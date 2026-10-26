// SysDVR-PS4: shows on the PS4 the picture and sound of a Switch running SysDVR (TCP Bridge mode).
#include "common.h"
#include "gfx.h"
#include "video.h"
#include "audio.h"
#include "sysdvr_proto.h"

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

typedef enum { UI_MAIN, UI_MENU, UI_IPEDIT } UiMode;

static UiMode g_ui = UI_MAIN;
static int g_menu_sel;
static int g_ip_octets[4];
static int g_ip_sel;
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
        bool manual = g_app.cfg.ip[0] != 0;
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

// ---------------------------------------------------------------- menu

enum {
    MI_IP, MI_FULLSCREEN, MI_SMOOTH, MI_FAST, MI_THREADS, MI_MAX_DELAY, MI_AUDIO, MI_AUDIO_BUF, MI_SCREEN_OFF, MI_RECONNECT, MI_CLOSE, MI_COUNT
};

static void menu_item_text(int i, const Config *c, char *out, size_t len)
{
    switch (i) {
    case MI_IP:         snprintf(out, len, "Switch IP: %s", c->ip[0] ? c->ip : "automatic"); break;
    case MI_FULLSCREEN: snprintf(out, len, "Fullscreen: %s", c->fullscreen ? "on" : "off (native 720p)"); break;
    case MI_SMOOTH:     snprintf(out, len, "Smooth scaling: %s", c->smooth ? "on" : "off (faster)"); break;
    case MI_FAST:       snprintf(out, len, "Fast decoding (lower quality): %s", c->fast ? "on" : "off"); break;
    case MI_THREADS:    snprintf(out, len, "Decoder threads: %d%s", c->threads, c->threads == 1 ? " (lowest latency)" : ""); break;
    case MI_MAX_DELAY:  snprintf(out, len, "Max video delay: %d ms", c->max_delay_ms); break;
    case MI_AUDIO:      snprintf(out, len, "Sound: %s", c->audio ? "on" : "off"); break;
    case MI_AUDIO_BUF:  snprintf(out, len, "Audio buffer: %d ms", c->audio_buffer_ms); break;
    case MI_SCREEN_OFF: snprintf(out, len, "Turn off Switch screen: %s", c->screen_off ? "on" : "off"); break;
    case MI_RECONNECT:  snprintf(out, len, "Reconnect"); break;
    case MI_CLOSE:      snprintf(out, len, "Close menu"); break;
    }
}

static void ip_editor_open(void)
{
    char src[64];
    pthread_mutex_lock(&g_app.lock);
    strcpy(src, g_app.cfg.ip[0] ? g_app.cfg.ip : (g_app.target_ip[0] ? g_app.target_ip : "192.168.1.10"));
    pthread_mutex_unlock(&g_app.lock);

    int a = 192, b = 168, c = 1, d = 10;
    sscanf(src, "%d.%d.%d.%d", &a, &b, &c, &d);
    g_ip_octets[0] = a; g_ip_octets[1] = b; g_ip_octets[2] = c; g_ip_octets[3] = d;
    g_ip_sel = 3;
    g_ui = UI_IPEDIT;
}

static void ui_update(void)
{
    if (g_ui == UI_MAIN) {
        if (pressed(ORBIS_PAD_BUTTON_OPTIONS)) {
            g_ui = UI_MENU;
            g_menu_sel = 0;
        }
        if (pressed(ORBIS_PAD_BUTTON_TRIANGLE))
            g_show_stats = !g_show_stats;
        if (pressed(ORBIS_PAD_BUTTON_SQUARE)) {
            pthread_mutex_lock(&g_app.lock);
            g_app.cfg.fullscreen = !g_app.cfg.fullscreen;
            pthread_mutex_unlock(&g_app.lock);
            config_save(&g_app.cfg);
        }
        if (pressed(ORBIS_PAD_BUTTON_CROSS) && g_app.video_state != LINK_STREAMING)
            reconnect();
        return;
    }

    if (g_ui == UI_IPEDIT) {
        if (pressed(ORBIS_PAD_BUTTON_LEFT))  g_ip_sel = (g_ip_sel + 3) % 4;
        if (pressed(ORBIS_PAD_BUTTON_RIGHT)) g_ip_sel = (g_ip_sel + 1) % 4;
        int delta = 0;
        if (pressed(ORBIS_PAD_BUTTON_UP))   delta += 1;
        if (pressed(ORBIS_PAD_BUTTON_DOWN)) delta -= 1;
        if (pressed(ORBIS_PAD_BUTTON_R1))   delta += 10;
        if (pressed(ORBIS_PAD_BUTTON_L1))   delta -= 10;
        if (delta)
            g_ip_octets[g_ip_sel] = (g_ip_octets[g_ip_sel] + delta + 256) % 256;

        if (pressed(ORBIS_PAD_BUTTON_CROSS)) {
            pthread_mutex_lock(&g_app.lock);
            snprintf(g_app.cfg.ip, sizeof(g_app.cfg.ip), "%d.%d.%d.%d", g_ip_octets[0], g_ip_octets[1], g_ip_octets[2], g_ip_octets[3]);
            pthread_mutex_unlock(&g_app.lock);
            config_save(&g_app.cfg);
            apply_target_from_config();
            g_ui = UI_MENU;
        }
        if (pressed(ORBIS_PAD_BUTTON_SQUARE)) {
            // back to automatic discovery
            pthread_mutex_lock(&g_app.lock);
            g_app.cfg.ip[0] = 0;
            pthread_mutex_unlock(&g_app.lock);
            config_save(&g_app.cfg);
            apply_target_from_config();
            g_ui = UI_MENU;
        }
        if (pressed(ORBIS_PAD_BUTTON_CIRCLE))
            g_ui = UI_MENU;
        return;
    }

    // UI_MENU
    if (pressed(ORBIS_PAD_BUTTON_UP))   g_menu_sel = (g_menu_sel + MI_COUNT - 1) % MI_COUNT;
    if (pressed(ORBIS_PAD_BUTTON_DOWN)) g_menu_sel = (g_menu_sel + 1) % MI_COUNT;

    int dir = 0;
    if (pressed(ORBIS_PAD_BUTTON_RIGHT) || pressed(ORBIS_PAD_BUTTON_CROSS)) dir = 1;
    if (pressed(ORBIS_PAD_BUTTON_LEFT)) dir = -1;

    if (pressed(ORBIS_PAD_BUTTON_CIRCLE) || pressed(ORBIS_PAD_BUTTON_OPTIONS)) {
        g_ui = UI_MAIN;
        return;
    }
    if (!dir)
        return;

    bool need_reconnect = false;
    bool cross = pressed(ORBIS_PAD_BUTTON_CROSS);
    pthread_mutex_lock(&g_app.lock);
    Config *c = &g_app.cfg;
    switch (g_menu_sel) {
    case MI_IP:
        if (cross) {
            pthread_mutex_unlock(&g_app.lock);
            ip_editor_open();
            return;
        }
        break;
    case MI_FULLSCREEN: c->fullscreen = !c->fullscreen; break;
    case MI_SMOOTH:     c->smooth = !c->smooth; break;
    case MI_MAX_DELAY:
        c->max_delay_ms += dir * 50;
        if (c->max_delay_ms < 50) c->max_delay_ms = 50;
        if (c->max_delay_ms > 1000) c->max_delay_ms = 1000;
        break;
    case MI_FAST:       c->fast = !c->fast; need_reconnect = true; break;
    case MI_THREADS:
        c->threads += dir;
        if (c->threads < 1) c->threads = 6;
        if (c->threads > 6) c->threads = 1;
        need_reconnect = true;
        break;
    case MI_AUDIO:      c->audio = !c->audio; break;
    case MI_AUDIO_BUF:
        c->audio_buffer_ms += dir * 20;
        if (c->audio_buffer_ms < 20) c->audio_buffer_ms = 20;
        if (c->audio_buffer_ms > 400) c->audio_buffer_ms = 400;
        break;
    case MI_SCREEN_OFF: c->screen_off = !c->screen_off; need_reconnect = true; break;
    case MI_RECONNECT:
        if (cross) need_reconnect = true;
        break;
    case MI_CLOSE:
        if (cross) g_ui = UI_MAIN;
        break;
    }
    Config saved = *c;
    pthread_mutex_unlock(&g_app.lock);

    config_save(&saved);
    if (need_reconnect)
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

static void draw_menu(uint32_t *fb)
{
    Config c;
    pthread_mutex_lock(&g_app.lock);
    c = g_app.cfg;
    pthread_mutex_unlock(&g_app.lock);

    int w = 1100, h = 120 + MI_COUNT * 60 + 80;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx_rect(fb, x - 4, y - 4, w + 8, h + 8, COL_ACCENT);
    gfx_rect(fb, x, y, w, h, COL_PANEL);
    gfx_text(fb, x + 40, y + 30, 5, COL_TEXT, "Settings");

    for (int i = 0; i < MI_COUNT; i++) {
        char line[128];
        menu_item_text(i, &c, line, sizeof(line));
        int ly = y + 120 + i * 60;
        if (i == g_menu_sel)
            gfx_rect(fb, x + 20, ly - 10, w - 40, 52, COL_ACCENT);
        gfx_text(fb, x + 40, ly, 4, COL_TEXT, line);
    }
    gfx_text(fb, x + 40, y + h - 55, 3, COL_DIM, "Up/Down: select   Left/Right/X: change   O: close");
}

static void draw_ip_editor(uint32_t *fb)
{
    int w = 1000, h = 420;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx_rect(fb, x - 4, y - 4, w + 8, h + 8, COL_ACCENT);
    gfx_rect(fb, x, y, w, h, COL_PANEL);
    gfx_text(fb, x + 40, y + 30, 5, COL_TEXT, "Switch IP address");

    int bx = x + 90;
    for (int i = 0; i < 4; i++) {
        char t[8];
        snprintf(t, sizeof(t), "%3d", g_ip_octets[i]);
        int ox = bx + i * 210;
        if (i == g_ip_sel)
            gfx_rect(fb, ox - 15, y + 135, 170, 110, COL_ACCENT);
        gfx_text(fb, ox, y + 150, 10, COL_TEXT, t);
        if (i < 3)
            gfx_text(fb, ox + 165, y + 150, 10, COL_TEXT, ".");
    }
    gfx_text(fb, x + 40, y + 290, 3, COL_DIM, "Left/Right: field   Up/Down: +-1   L1/R1: +-10");
    gfx_text(fb, x + 40, y + 340, 3, COL_DIM, "X: save   SQUARE: automatic discovery   O: cancel");
}

static void draw_status_screen(uint32_t *fb)
{
    char ip[64], serial[64], msg[256];
    LinkState vs, as;
    bool manual, audio;
    pthread_mutex_lock(&g_app.lock);
    strcpy(ip, g_app.target_ip);
    strcpy(serial, g_app.serial);
    strcpy(msg, g_app.message);
    vs = g_app.video_state;
    as = g_app.audio_state;
    manual = g_app.cfg.ip[0] != 0;
    audio = g_app.cfg.audio;
    pthread_mutex_unlock(&g_app.lock);

    gfx_clear(fb, COL_BG);
    gfx_rect(fb, 0, 0, SCREEN_W, 12, COL_ACCENT);
    gfx_text_centered(fb, 110, 9, COL_TEXT, "SysDVR for PS4");
    gfx_text_centered(fb, 230, 3, COL_DIM, "Play your Nintendo Switch on the TV through your PS4 - v" APP_VERSION);

    char line[256];
    int y = 340;
    if (!ip[0]) {
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
             manual ? "manual IP" : "auto discovery", state_name(vs), audio ? state_name(as) : "off",
             serial[0] ? "   |   Switch: " : "", serial);
    gfx_text_centered(fb, y + 200, 3, COL_DIM, line);

    int bx = 300, by = 650;
    gfx_rect(fb, bx - 30, by - 30, SCREEN_W - 2 * (bx - 30), 300, COL_PANEL);
    gfx_text(fb, bx, by, 4, COL_TEXT, "1. On the Switch: open SysDVR Settings and pick TCP Bridge mode");
    gfx_text(fb, bx, by + 60, 4, COL_TEXT, "2. Connect the Switch and the PS4 to the same network (same router)");
    gfx_text(fb, bx, by + 120, 4, COL_TEXT, "3. Start a game on the Switch: the picture shows up here");
    gfx_text(fb, bx, by + 180, 3, COL_DIM, "If the Switch is not found, enter its IP in OPTIONS > Switch IP");

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
    if (g_ui == UI_MENU)
        draw_menu(fb);
    else if (g_ui == UI_IPEDIT)
        draw_ip_editor(fb);
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
        bool smooth = g_app.cfg.smooth;
        pthread_mutex_unlock(&g_app.lock);

        bool showing_video = streaming && g_app.last_frame_us && (t - g_app.last_frame_us) < 3000000;
        if (showing_video && !was_showing_video)
            g_hint_until = t + 5000000;
        was_showing_video = showing_video;

        uint32_t *fb = gfx_backbuffer();
        if (showing_video) {
            // show every new frame as soon as it is decoded; redraw the last one when the menu changes
            bool force = input || t - last_flip > 100000;
            if ((force || video_has_new_frame()) && video_render(fb, fullscreen, smooth, force)) {
                draw_overlays(fb);
                gfx_flip();
                last_flip = t;
            } else {
                sleep_ms(1);
            }
        } else if (input || t - last_flip > 50000) {
            // also drains leftover frames
            video_render(fb, fullscreen, smooth, false);
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
