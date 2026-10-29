// SysDVR-PS4: shows on the PS4 the picture and sound of a Switch running SysDVR (TCP Bridge mode).
#include "common.h"
#include "gfx.h"
#include "video.h"
#include "audio.h"
#include "sysdvr_proto.h"
#include "usb.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#define COL_BG      RGB(18, 18, 24)
#define COL_PANEL   RGB(32, 32, 42)
#define COL_TEXT    RGB(235, 235, 235)
#define COL_DIM     RGB(150, 150, 160)
#define COL_ACCENT  RGB(230, 0, 18)
#define COL_OK      RGB(80, 200, 120)
#define COL_WARN    RGB(240, 180, 60)

// ---------------------------------------------------------------- controller

static int g_pad = -1;
static uint32_t g_buttons, g_pressed;
static uint32_t g_repeat_btn;
static uint64_t g_repeat_at;

static void pad_init(void)
{
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);

    if (scePadInit() != 0) {
        log_printf("scePadInit failed");
        return;
    }
    int user = -1;
    sceUserServiceGetInitialUser(&user);
    g_pad = scePadOpen(user, 0, 0, NULL);
    if (g_pad < 0)
        log_printf("scePadOpen failed: %x", g_pad);
}

static void pad_update(void)
{
    uint32_t now = 0;
    if (g_pad >= 0) {
        OrbisPadData data;
        if (scePadReadState(g_pad, &data) == 0 && data.connected)
            now = data.buttons;
    }
    g_pressed = now & ~g_buttons;
    g_buttons = now;

    // auto-repeat for the D-pad and L1/R1
    const uint32_t rep = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                         ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1;
    uint64_t t = now_us();
    if (g_pressed & rep) {
        g_repeat_btn = g_pressed & rep;
        g_repeat_at = t + 400000;
    } else if (g_repeat_btn && (g_buttons & g_repeat_btn)) {
        if (t >= g_repeat_at) {
            g_pressed |= g_repeat_btn & g_buttons;
            g_repeat_at = t + 70000;
        }
    } else {
        g_repeat_btn = 0;
    }
}

static bool pressed(uint32_t b) { return (g_pressed & b) != 0; }

// ---------------------------------------------------------------- application state

static bool g_show_stats;
static uint64_t g_hint_until;

static void reconnect(void)
{
    pthread_mutex_lock(&g_app.lock);
    g_app.generation++;
    pthread_mutex_unlock(&g_app.lock);
    video_kick();
    audio_kick();
}

static void apply_target_from_config(void)
{
    pthread_mutex_lock(&g_app.lock);
    if (g_app.cfg.ip[0])
        strcpy(g_app.target_ip, g_app.cfg.ip);
    else
        g_app.target_ip[0] = 0; // filled in by discovery
    pthread_mutex_unlock(&g_app.lock);
    reconnect();
}

// Automatic discovery: the Switch announces itself over UDP on port 19999 every 2 s
static void *discovery_thread(void *arg)
{
    (void)arg;
    while (!g_app.quit) {
        pthread_mutex_lock(&g_app.lock);
        bool manual = g_app.cfg.ip[0] != 0 || g_app.cfg.usb;
        bool streaming = g_app.video_state == LINK_STREAMING;
        pthread_mutex_unlock(&g_app.lock);

        if (manual || streaming) {
            sleep_ms(500);
            continue;
        }

        char ip[64] = { 0 }, serial[64] = { 0 };
        if (sdvr_discover(2500, ip, sizeof(ip), serial, sizeof(serial))) {
            bool changed = false;
            pthread_mutex_lock(&g_app.lock);
            if (!g_app.cfg.ip[0] && strcmp(g_app.target_ip, ip) != 0) {
                strcpy(g_app.target_ip, ip);
                snprintf(g_app.serial, sizeof(g_app.serial), "%s", serial);
                g_app.generation++;
                changed = true;
            }
            pthread_mutex_unlock(&g_app.lock);
            if (changed)
                log_printf("Switch found: %s (serial %s)", ip, serial);
        } else {
            sleep_ms(100);
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- input

static uint32_t pad_keys(void)
{
    static const struct { uint32_t pad, key; } MAP[] = {
        { ORBIS_PAD_BUTTON_UP, KEY_UP }, { ORBIS_PAD_BUTTON_DOWN, KEY_DOWN },
        { ORBIS_PAD_BUTTON_LEFT, KEY_LEFT }, { ORBIS_PAD_BUTTON_RIGHT, KEY_RIGHT },
        { ORBIS_PAD_BUTTON_CROSS, KEY_CONFIRM }, { ORBIS_PAD_BUTTON_CIRCLE, KEY_BACK },
        { ORBIS_PAD_BUTTON_SQUARE, KEY_ALT }, { ORBIS_PAD_BUTTON_L1, KEY_L1 },
        { ORBIS_PAD_BUTTON_R1, KEY_R1 }, { ORBIS_PAD_BUTTON_OPTIONS, KEY_MENU },
    };
    uint32_t keys = 0;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (g_pressed & MAP[i].pad)
            keys |= MAP[i].key;
    return keys;
}

static void ui_update(void)
{
    uint32_t keys = pad_keys();
    if (menu_is_open()) {
        switch (menu_input(keys)) {
        case MENU_RECONNECT:      reconnect(); break;
        case MENU_TARGET_CHANGED: apply_target_from_config(); break;
        case MENU_NONE:           break;
        }
        return;
    }

    if (keys & KEY_MENU)
        menu_open();
    if (pressed(ORBIS_PAD_BUTTON_TRIANGLE))
        g_show_stats = !g_show_stats;
    if (keys & KEY_ALT) {
        pthread_mutex_lock(&g_app.lock);
        g_app.cfg.fullscreen = !g_app.cfg.fullscreen;
        Config saved = g_app.cfg;
        pthread_mutex_unlock(&g_app.lock);
        config_save(&saved);
    }
    if ((keys & KEY_CONFIRM) && g_app.video_state != LINK_STREAMING)
        reconnect();
}

// ---------------------------------------------------------------- drawing

static const char *state_name(LinkState s)
{
    switch (s) {
    case LINK_IDLE: return "idle";
    case LINK_CONNECTING: return "connecting...";
    case LINK_STREAMING: return "connected";
    case LINK_ERROR: return "error";
    }
    return "?";
}

static void draw_status_screen(uint32_t *fb)
{
    char ip[64], serial[64], msg[256];
    LinkState vs, as;
    bool manual, audio, usb, usb_connected;
    pthread_mutex_lock(&g_app.lock);
    strcpy(ip, g_app.target_ip);
    strcpy(serial, g_app.serial);
    strcpy(msg, g_app.message);
    vs = g_app.video_state;
    as = g_app.audio_state;
    manual = g_app.cfg.ip[0] != 0;
    audio = g_app.cfg.audio;
    usb = g_app.cfg.usb;
    usb_connected = g_app.usb_connected;
    pthread_mutex_unlock(&g_app.lock);

    gfx_clear(fb, COL_BG);
    gfx_rect(fb, 0, 0, SCREEN_W, 12, COL_ACCENT);
    gfx_text_centered(fb, 110, 9, COL_TEXT, "SysDVR for PS4");
    gfx_text_centered(fb, 230, 3, COL_DIM, "Play your Nintendo Switch on the TV through your PS4 - v" APP_VERSION);

    char line[256];
    int y = 340;
    if (usb) {
        if (vs == LINK_STREAMING) {
            gfx_text_centered(fb, y, 5, COL_OK, "Connected to the Switch over USB");
            gfx_text_centered(fb, y + 80, 4, COL_WARN, "Waiting for video: start a game on the Switch");
        } else {
            gfx_text_centered(fb, y, 5, usb_connected ? COL_TEXT : COL_WARN,
                              usb_connected ? "Switch found on USB, connecting..." : "Plug the Switch into the PS4 with a USB cable");
        }
    } else if (!ip[0]) {
        gfx_text_centered(fb, y, 5, COL_WARN, "Searching for the Switch on the network...");
    } else if (vs == LINK_STREAMING) {
        snprintf(line, sizeof(line), "Connected to %s", ip);
        gfx_text_centered(fb, y, 5, COL_OK, line);
        gfx_text_centered(fb, y + 80, 4, COL_WARN, "Waiting for video: start a game on the Switch");
    } else {
        snprintf(line, sizeof(line), "Connecting to %s ...", ip);
        gfx_text_centered(fb, y, 5, vs == LINK_ERROR ? COL_WARN : COL_TEXT, line);
    }

    if (msg[0])
        gfx_text_centered(fb, y + 150, 3, COL_DIM, msg);

    snprintf(line, sizeof(line), "Mode: %s   |   Video: %s   |   Sound: %s%s%s",
             usb ? "USB" : manual ? "manual IP" : "auto discovery", state_name(vs), audio ? state_name(as) : "off",
             serial[0] ? "   |   Switch: " : "", serial);
    gfx_text_centered(fb, y + 200, 3, COL_DIM, line);

    int bx = 300, by = 650;
    gfx_rect(fb, bx - 30, by - 30, SCREEN_W - 2 * (bx - 30), 300, COL_PANEL);
    if (usb) {
        gfx_text(fb, bx, by, 4, COL_TEXT, "1. On the Switch: open SysDVR Settings and pick USB mode");
        gfx_text(fb, bx, by + 60, 4, COL_TEXT, "2. Plug the Switch into a PS4 USB port (USB-C to USB-A data cable)");
        gfx_text(fb, bx, by + 120, 4, COL_TEXT, "3. Start a game on the Switch: the picture shows up here");
        gfx_text(fb, bx, by + 180, 3, COL_DIM, "To stream over Wi-Fi instead: OPTIONS > Connection");
    } else {
        gfx_text(fb, bx, by, 4, COL_TEXT, "1. On the Switch: open SysDVR Settings and pick TCP Bridge mode");
        gfx_text(fb, bx, by + 60, 4, COL_TEXT, "2. Connect the Switch and the PS4 to the same network (same router)");
        gfx_text(fb, bx, by + 120, 4, COL_TEXT, "3. Start a game on the Switch: the picture shows up here");
        gfx_text(fb, bx, by + 180, 3, COL_DIM, "Not found? Enter its IP in OPTIONS > Switch IP, or use OPTIONS > Connection: USB");
    }

    gfx_text_centered(fb, 1010, 3, COL_DIM, "OPTIONS: settings     X: reconnect     TRIANGLE: stats     SQUARE: fullscreen");
}

static void draw_stats(uint32_t *fb)
{
    static uint64_t last_t;
    static uint32_t last_frames;
    static uint64_t last_bytes;
    static char line1[128] = "", line2[128] = "";

    uint64_t t = now_us();
    if (t - last_t >= 500000) {
        double dt = (t - last_t) / 1e6;
        uint32_t f = g_app.video_frames;
        uint64_t b = g_app.video_bytes;
        snprintf(line1, sizeof(line1), "%.1f fps   %.2f Mbit/s   decode %.1f ms   delay +%d ms",
                 (f - last_frames) / dt, (b - last_bytes) * 8 / dt / 1e6, g_app.decode_us / 1000.0,
                 g_app.video_delay_ms > 0 ? (int)g_app.video_delay_ms : 0);
        snprintf(line2, sizeof(line2), "queue %u   skipped %u   audio %u ms   underruns %u",
                 g_app.video_queue, g_app.video_dropped, g_app.audio_fill_ms, g_app.audio_underruns);
        last_t = t;
        last_frames = f;
        last_bytes = b;
    }
    int w1 = gfx_text_width(line1, 3), w2 = gfx_text_width(line2, 3);
    gfx_rect(fb, 20, 20, (w1 > w2 ? w1 : w2) + 30, 90, RGB(0, 0, 0));
    gfx_text(fb, 35, 32, 3, COL_OK, line1);
    gfx_text(fb, 35, 72, 3, COL_DIM, line2);
}

static void draw_overlays(uint32_t *fb)
{
    if (g_show_stats)
        draw_stats(fb);
    if (now_us() < g_hint_until) {
        const char *hint = "OPTIONS: menu    TRIANGLE: stats    SQUARE: fullscreen";
        int w = gfx_text_width(hint, 3) + 40;
        gfx_rect(fb, (SCREEN_W - w) / 2, SCREEN_H - 90, w, 50, RGB(0, 0, 0));
        gfx_text_centered(fb, SCREEN_H - 78, 3, COL_TEXT, hint);
    }
    menu_draw(fb);
}

// ---------------------------------------------------------------- main

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&g_app, 0, sizeof(g_app));
    pthread_mutex_init(&g_app.lock, NULL);

    log_init();
    config_load(&g_app.cfg);
    strcpy(g_app.message, "");

    sceSystemServiceHideSplashScreen();

    if (!gfx_init()) {
        log_printf("Graphics initialisation failed");
        for (;;)
            sleep_ms(1000);
    }

    pad_init();

    apply_target_from_config();

    pthread_t disc;
    pthread_create(&disc, NULL, discovery_thread, NULL);

    if (!video_start())
        app_set_message("Could not start the video threads");
    if (!usb_start())
        log_printf("Could not start the USB thread");
    if (!audio_start())
        log_printf("Audio unavailable");

    uint64_t last_tick = 0, last_flip = 0;
    bool was_showing_video = false;

    for (;;) {
        pad_update();
        bool input = g_pressed != 0;
        ui_update();

        uint64_t t = now_us();
        if (t - last_tick > 5000000) {
            // keeps the PS4 from dimming the screen / going to sleep
            sceSystemServicePowerTick();
            last_tick = t;
        }

        pthread_mutex_lock(&g_app.lock);
        bool streaming = g_app.video_state == LINK_STREAMING;
        bool fullscreen = g_app.cfg.fullscreen;
        int scaler = g_app.cfg.scaler;
        pthread_mutex_unlock(&g_app.lock);

        bool showing_video = streaming && g_app.last_frame_us && (t - g_app.last_frame_us) < 3000000;
        if (showing_video && !was_showing_video)
            g_hint_until = t + 5000000;
        was_showing_video = showing_video;

        uint32_t *fb = gfx_backbuffer();
        if (showing_video) {
            // show every new frame as soon as it is decoded; redraw the last one when the menu changes
            bool force = input || t - last_flip > 100000;
            if ((force || video_has_new_frame()) && video_render(fb, fullscreen, scaler, force)) {
                draw_overlays(fb);
                gfx_flip();
                last_flip = t;
            } else {
                sleep_ms(1);
            }
        } else if (input || t - last_flip > 50000) {
            // also drains leftover frames
            video_render(fb, fullscreen, scaler, false);
            draw_status_screen(fb);
            draw_overlays(fb);
            gfx_flip();
            last_flip = t;
        } else {
            sleep_ms(5);
        }
    }
    return 0;
}
