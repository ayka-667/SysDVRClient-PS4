// Settings menu: a short list of plain choices. The fine-grained knobs (threads, buffers...) stay in config.ini
// and are set together through the "Performance" presets.
#include "menu.h"
#include "common.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>

#define COL_SHADE    RGB(10, 10, 14)
#define COL_CARD     RGB(28, 28, 36)
#define COL_ROW_SEL  RGB(46, 46, 60)
#define COL_LINE     RGB(40, 40, 52)
#define COL_TEXT     RGB(240, 240, 245)
#define COL_LABEL    RGB(165, 165, 180)
#define COL_HELP     RGB(130, 130, 145)
#define COL_ACCENT   RGB(230, 0, 18)
#define COL_VALUE_OK RGB(110, 210, 140)

typedef enum { ITEM_CONNECTION, ITEM_IP, ITEM_PICTURE, ITEM_PERFORMANCE, ITEM_SOUND, ITEM_SCREEN, ITEM_RECONNECT } Item;

typedef enum { VIEW_CLOSED, VIEW_LIST, VIEW_IP } View;

static View g_view = VIEW_CLOSED;
static int g_sel;
static int g_ip[4];
static int g_ip_field;

// ---------------------------------------------------------------- performance presets

typedef struct {
    const char *name;
    const char *help;
    int threads;
    bool fast;
    int scaler;
    int max_delay_ms;
    int audio_buffer_ms;
} Preset;

static const Preset PRESETS[] = {
    { "Low latency",  "Fastest response. The picture is a little softer and blockier.", 1, true,  SCALER_FAST,   100, 40 },
    { "Balanced",     "Good picture with low delay. Recommended.",                       2, false, SCALER_SMOOTH, 150, 60 },
    { "Best quality", "Sharpest picture, about one frame more delay. Needs a fast PS4.", 3, false, SCALER_SHARP,  250, 80 },
};
#define PRESET_COUNT ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

// -1 when config.ini was edited by hand to something else
static int preset_of(const Config *c)
{
    for (int i = 0; i < PRESET_COUNT; i++) {
        const Preset *p = &PRESETS[i];
        if (c->threads == p->threads && c->fast == p->fast && c->scaler == p->scaler &&
            c->max_delay_ms == p->max_delay_ms && c->audio_buffer_ms == p->audio_buffer_ms)
            return i;
    }
    return -1;
}

static void apply_preset(Config *c, int i)
{
    const Preset *p = &PRESETS[i];
    c->threads = p->threads;
    c->fast = p->fast;
    c->scaler = p->scaler;
    c->max_delay_ms = p->max_delay_ms;
    c->audio_buffer_ms = p->audio_buffer_ms;
}

// ---------------------------------------------------------------- items

// visible items in order; the IP is hidden over USB
static int visible_items(const Config *c, Item *out)
{
    int n = 0;
    out[n++] = ITEM_CONNECTION;
    if (!c->usb)
        out[n++] = ITEM_IP;
    out[n++] = ITEM_PICTURE;
    out[n++] = ITEM_PERFORMANCE;
    out[n++] = ITEM_SOUND;
    out[n++] = ITEM_SCREEN;
    out[n++] = ITEM_RECONNECT;
    return n;
}

static const char *item_label(Item it)
{
    switch (it) {
    case ITEM_CONNECTION:  return "Connection";
    case ITEM_IP:          return "Switch IP";
    case ITEM_PICTURE:     return "Picture";
    case ITEM_PERFORMANCE: return "Performance";
    case ITEM_SOUND:       return "Sound";
    case ITEM_SCREEN:      return "Switch screen";
    case ITEM_RECONNECT:   return "Reconnect";
    }
    return "";
}

static void item_value(Item it, const Config *c, char *out, size_t len)
{
    int p;
    switch (it) {
    case ITEM_CONNECTION:  snprintf(out, len, "%s", c->usb ? "USB cable" : "Wi-Fi"); break;
    case ITEM_IP:          snprintf(out, len, "%s", c->ip[0] ? c->ip : "Automatic"); break;
    case ITEM_PICTURE:     snprintf(out, len, "%s", c->fullscreen ? "Fullscreen" : "Original size"); break;
    case ITEM_PERFORMANCE: p = preset_of(c); snprintf(out, len, "%s", p >= 0 ? PRESETS[p].name : "Custom"); break;
    case ITEM_SOUND:       snprintf(out, len, "%s", c->audio ? "On" : "Off"); break;
    case ITEM_SCREEN:      snprintf(out, len, "%s", c->screen_off ? "Off while streaming" : "On"); break;
    case ITEM_RECONNECT:   out[0] = 0; break;
    }
}

static const char *item_help(Item it, const Config *c)
{
    int p;
    switch (it) {
    case ITEM_CONNECTION:
        return c->usb ? "Switch plugged into the PS4. Set SysDVR to USB mode on the Switch."
                      : "Switch and PS4 on the same network. Set SysDVR to TCP Bridge mode on the Switch.";
    case ITEM_IP:          return "Automatic finds the Switch by itself. Press X to type its IP if it is not found.";
    case ITEM_PICTURE:     return c->fullscreen ? "Stretched to fill the TV (1080p)." : "Pixel-perfect 720p in the middle of the screen.";
    case ITEM_PERFORMANCE: p = preset_of(c); return p >= 0 ? PRESETS[p].help : "Set by hand in config.ini. Pick a preset to replace it.";
    case ITEM_SOUND:       return "Play the Switch's sound through the PS4.";
    case ITEM_SCREEN:      return "Turn off the Switch's own screen while streaming to save battery. Needs SysDVR 6.1+.";
    case ITEM_RECONNECT:   return "Close the connection and connect again.";
    }
    return "";
}

static bool item_is_choice(Item it)
{
    return it != ITEM_RECONNECT && it != ITEM_IP;
}

// ---------------------------------------------------------------- input

void menu_open(void)
{
    g_view = VIEW_LIST;
    g_sel = 0;
}

bool menu_is_open(void)
{
    return g_view != VIEW_CLOSED;
}

static void ip_open(void)
{
    char src[64];
    pthread_mutex_lock(&g_app.lock);
    snprintf(src, sizeof(src), "%s", g_app.cfg.ip[0] ? g_app.cfg.ip : (g_app.target_ip[0] ? g_app.target_ip : "192.168.1.10"));
    pthread_mutex_unlock(&g_app.lock);

    int a = 192, b = 168, c = 1, d = 10;
    sscanf(src, "%d.%d.%d.%d", &a, &b, &c, &d);
    g_ip[0] = a; g_ip[1] = b; g_ip[2] = c; g_ip[3] = d;
    g_ip_field = 3;
    g_view = VIEW_IP;
}

static MenuAction ip_input(uint32_t keys)
{
    if (keys & KEY_LEFT)  g_ip_field = (g_ip_field + 3) % 4;
    if (keys & KEY_RIGHT) g_ip_field = (g_ip_field + 1) % 4;
    int delta = 0;
    if (keys & KEY_UP)   delta += 1;
    if (keys & KEY_DOWN) delta -= 1;
    if (keys & KEY_R1)   delta += 10;
    if (keys & KEY_L1)   delta -= 10;
    if (delta)
        g_ip[g_ip_field] = (g_ip[g_ip_field] + delta + 256) % 256;

    if (keys & KEY_BACK) {
        g_view = VIEW_LIST;
        return MENU_NONE;
    }
    if (!(keys & (KEY_CONFIRM | KEY_ALT)))
        return MENU_NONE;

    pthread_mutex_lock(&g_app.lock);
    if (keys & KEY_ALT)
        g_app.cfg.ip[0] = 0; // back to automatic
    else
        snprintf(g_app.cfg.ip, sizeof(g_app.cfg.ip), "%d.%d.%d.%d", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
    Config saved = g_app.cfg;
    pthread_mutex_unlock(&g_app.lock);
    config_save(&saved);
    g_view = VIEW_LIST;
    return MENU_TARGET_CHANGED;
}

MenuAction menu_input(uint32_t keys)
{
    if (g_view == VIEW_CLOSED)
        return MENU_NONE;
    if (g_view == VIEW_IP)
        return ip_input(keys);

    if (keys & (KEY_BACK | KEY_MENU)) {
        g_view = VIEW_CLOSED;
        return MENU_NONE;
    }

    pthread_mutex_lock(&g_app.lock);
    Config *c = &g_app.cfg;
    Item items[8];
    int n = visible_items(c, items);
    if (keys & KEY_UP)   g_sel = (g_sel + n - 1) % n;
    if (keys & KEY_DOWN) g_sel = (g_sel + 1) % n;
    if (g_sel >= n) g_sel = n - 1;
    Item it = items[g_sel];

    int dir = 0;
    if (keys & (KEY_RIGHT | KEY_CONFIRM)) dir = 1;
    if (keys & KEY_LEFT) dir = -1;
    bool confirm = (keys & KEY_CONFIRM) != 0;

    MenuAction action = MENU_NONE;
    bool changed = false;
    if (dir && item_is_choice(it)) {
        changed = true;
        switch (it) {
        case ITEM_CONNECTION:  c->usb = !c->usb; action = MENU_RECONNECT; break;
        case ITEM_PICTURE:     c->fullscreen = !c->fullscreen; break;
        case ITEM_SOUND:       c->audio = !c->audio; break;
        case ITEM_SCREEN:      c->screen_off = !c->screen_off; action = MENU_RECONNECT; break;
        case ITEM_PERFORMANCE: {
            int p = preset_of(c);
            p = p < 0 ? 1 : (p + dir + PRESET_COUNT) % PRESET_COUNT;
            apply_preset(c, p);
            action = MENU_RECONNECT;
            break;
        }
        default: break;
        }
    }
    Config saved = *c;
    pthread_mutex_unlock(&g_app.lock);

    if (changed)
        config_save(&saved);
    if (confirm && it == ITEM_IP)
        ip_open();
    if (confirm && it == ITEM_RECONNECT)
        action = MENU_RECONNECT;
    return action;
}

// ---------------------------------------------------------------- drawing

#define CARD_W   1120
#define ROW_H    84
#define PAD      56

static void draw_card(uint32_t *fb, int x, int y, int w, int h, const char *title)
{
    gfx_rect(fb, x + 10, y + 12, w, h, COL_SHADE); // drop shadow
    gfx_rect(fb, x, y, w, h, COL_CARD);
    gfx_rect(fb, x, y, w, 8, COL_ACCENT);
    gfx_text(fb, x + PAD, y + 48, 6, COL_TEXT, title);
}

// a small rounded-looking key cap with a letter, then its meaning
static int draw_hint(uint32_t *fb, int x, int y, const char *key, const char *what)
{
    int kw = gfx_text_width(key, 3) + 22;
    gfx_rect(fb, x, y - 8, kw, 38, COL_ROW_SEL);
    gfx_text(fb, x + 11, y, 3, COL_TEXT, key);
    x += kw + 14;
    gfx_text(fb, x, y, 3, COL_LABEL, what);
    return x + gfx_text_width(what, 3) + 44;
}

// word-wrapped text, at most max_lines lines
static void draw_wrapped(uint32_t *fb, int x, int y, int max_w, int scale, int line_h, int max_lines, uint32_t color, const char *text)
{
    char line[256] = "";
    const char *p = text;
    while (*p && max_lines > 0) {
        const char *sp = strchr(p, ' ');
        size_t wl = sp ? (size_t)(sp - p) : strlen(p);
        char candidate[256];
        size_t ll = strlen(line);
        if (ll + 1 + wl >= sizeof(candidate))
            break;
        memcpy(candidate, line, ll);
        size_t cl = ll;
        if (ll)
            candidate[cl++] = ' ';
        memcpy(candidate + cl, p, wl);
        candidate[cl + wl] = 0;
        if (line[0] && gfx_text_width(candidate, scale) > max_w) {
            gfx_text(fb, x, y, scale, color, line);
            y += line_h;
            max_lines--;
            memcpy(line, p, wl);
            line[wl] = 0;
        } else {
            memcpy(line, candidate, cl + wl + 1);
        }
        p += wl;
        while (*p == ' ')
            p++;
    }
    if (line[0] && max_lines > 0)
        gfx_text(fb, x, y, scale, color, line);
}

static void draw_list(uint32_t *fb)
{
    Config c;
    pthread_mutex_lock(&g_app.lock);
    c = g_app.cfg;
    pthread_mutex_unlock(&g_app.lock);

    Item items[8];
    int n = visible_items(&c, items);
    if (g_sel >= n) g_sel = n - 1;

    int list_y = 150;
    int help_h = 140;
    int h = list_y + n * ROW_H + 24 + help_h + 90;
    int x = (SCREEN_W - CARD_W) / 2, y = (SCREEN_H - h) / 2;
    draw_card(fb, x, y, CARD_W, h, "Settings");

    for (int i = 0; i < n; i++) {
        Item it = items[i];
        int ry = y + list_y + i * ROW_H;
        bool sel = i == g_sel;
        if (sel) {
            gfx_rect(fb, x + 24, ry, CARD_W - 48, ROW_H - 6, COL_ROW_SEL);
            gfx_rect(fb, x + 24, ry, 8, ROW_H - 6, COL_ACCENT);
        } else if (i > 0) {
            gfx_rect(fb, x + PAD, ry - 4, CARD_W - 2 * PAD, 2, COL_LINE);
        }
        int ty = ry + 24;
        gfx_text(fb, x + PAD + 10, ty, 4, sel ? COL_TEXT : COL_LABEL, item_label(it));

        char val[64];
        item_value(it, &c, val, sizeof(val));
        int right = x + CARD_W - PAD - 10;
        if (it == ITEM_IP || it == ITEM_RECONNECT) {
            // actions: a plain "X" cue instead of arrows
            if (sel) {
                const char *cue = it == ITEM_IP ? "Edit  X" : "Press  X";
                int cw = gfx_text_width(cue, 3);
                gfx_text(fb, right - cw, ty + 6, 3, COL_LABEL, cue);
                right -= cw + 30;
            }
            if (val[0])
                gfx_text(fb, right - gfx_text_width(val, 4), ty, 4, COL_TEXT, val);
        } else if (sel) {
            int vw = gfx_text_width(val, 4);
            gfx_text(fb, right - gfx_text_width(">", 4), ty, 4, COL_ACCENT, ">");
            gfx_text(fb, right - 40 - vw, ty, 4, COL_TEXT, val);
            gfx_text(fb, right - 80 - vw - gfx_text_width("<", 4), ty, 4, COL_ACCENT, "<");
        } else {
            gfx_text(fb, right - gfx_text_width(val, 4), ty, 4, COL_TEXT, val);
        }
    }

    // help for the selected item
    int hy = y + list_y + n * ROW_H + 24;
    gfx_rect(fb, x + PAD, hy, CARD_W - 2 * PAD, 2, COL_LINE);
    draw_wrapped(fb, x + PAD, hy + 30, CARD_W - 2 * PAD, 3, 38, 2, COL_HELP, item_help(items[g_sel], &c));

    int fy = y + h - 64;
    int fx = x + PAD;
    fx = draw_hint(fb, fx, fy, "Up/Down", "Move");
    fx = draw_hint(fb, fx, fy, "Left/Right", "Change");
    draw_hint(fb, fx, fy, "O", "Close");
}

static void draw_ip(uint32_t *fb)
{
    int w = CARD_W, h = 560;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    draw_card(fb, x, y, w, h, "Switch IP");
    gfx_text(fb, x + PAD, y + 130, 3, COL_HELP, "On the Switch: System Settings > Internet shows its IP address.");

    const int fw = 200, gap = 50;
    int total = 4 * fw + 3 * gap;
    int fx0 = x + (w - total) / 2;
    int fy = y + 230;
    for (int i = 0; i < 4; i++) {
        int fx = fx0 + i * (fw + gap);
        bool sel = i == g_ip_field;
        gfx_rect(fb, fx, fy, fw, 130, sel ? COL_ROW_SEL : COL_SHADE);
        if (sel) {
            gfx_rect(fb, fx, fy + 124, fw, 6, COL_ACCENT);
            gfx_text(fb, fx + (fw - gfx_text_width("+", 4)) / 2, fy - 50, 4, COL_ACCENT, "+");
            gfx_text(fb, fx + (fw - gfx_text_width("-", 4)) / 2, fy + 150, 4, COL_ACCENT, "-");
        }
        char t[8];
        snprintf(t, sizeof(t), "%d", g_ip[i]);
        gfx_text(fb, fx + (fw - gfx_text_width(t, 9)) / 2, fy + 30, 9, sel ? COL_TEXT : COL_LABEL, t);
        if (i < 3)
            gfx_text(fb, fx + fw + (gap - gfx_text_width(".", 9)) / 2, fy + 30, 9, COL_LABEL, ".");
    }

    int fy2 = y + h - 64;
    int fx = x + PAD;
    fx = draw_hint(fb, fx, fy2, "X", "Save");
    fx = draw_hint(fb, fx, fy2, "Square", "Automatic");
    fx = draw_hint(fb, fx, fy2, "L1/R1", "-10/+10");
    draw_hint(fb, fx, fy2, "O", "Cancel");
}

void menu_draw(uint32_t *fb)
{
    if (g_view == VIEW_LIST)
        draw_list(fb);
    else if (g_view == VIEW_IP)
        draw_ip(fb);
}
