// Software drawing into a 1920x1080 BGRA framebuffer. No PS4 dependency, so the native UI preview can use it too.
#include "gfx.h"

#include <stb/stb_easy_font.h>

void gfx_clear(uint32_t *fb, uint32_t color)
{
    gfx_rect(fb, 0, 0, SCREEN_W, SCREEN_H, color);
}

void gfx_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = fb + (size_t)(y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

typedef struct {
    float x, y, z;
    unsigned char c[4];
} EasyFontVertex;

void gfx_text(uint32_t *fb, int x, int y, int scale, uint32_t color, const char *text)
{
    static EasyFontVertex verts[4 * 2000];
    // static vertex buffer: main thread only
    int quads = stb_easy_font_print(0, 0, (char *)text, NULL, verts, sizeof(verts));
    for (int q = 0; q < quads; q++) {
        EasyFontVertex *v = &verts[q * 4];
        int x0 = (int)(v[0].x * scale), y0 = (int)(v[0].y * scale);
        int x1 = (int)(v[2].x * scale), y1 = (int)(v[2].y * scale);
        gfx_rect(fb, x + x0, y + y0, x1 - x0, y1 - y0, color);
    }
}

int gfx_text_width(const char *text, int scale)
{
    return stb_easy_font_width((char *)text) * scale;
}

void gfx_text_centered(uint32_t *fb, int y, int scale, uint32_t color, const char *text)
{
    gfx_text(fb, (SCREEN_W - gfx_text_width(text, scale)) / 2, y, scale, color, text);
}
