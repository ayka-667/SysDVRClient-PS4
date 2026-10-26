#include "gfx.h"
#include "common.h"

#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

#include <stb/stb_easy_font.h>

#define NUM_BUFFERS 2
#define FB_SIZE (SCREEN_W * SCREEN_H * 4)

static int g_video = -1;
static OrbisKernelEqueue g_flip_queue;
static uint32_t *g_buffers[NUM_BUFFERS];
static int g_current;
static int64_t g_frame_id;

bool gfx_init(void)
{
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g_video < 0) {
        log_printf("sceVideoOutOpen a echoue : %x", g_video);
        return false;
    }

    if (sceKernelCreateEqueue(&g_flip_queue, "sysdvr flip queue") < 0) {
        log_printf("sceKernelCreateEqueue a echoue");
        return false;
    }
    sceVideoOutAddFlipEvent(g_flip_queue, g_video, 0);

    const size_t align = 0x200000;
    size_t total = ((size_t)FB_SIZE * NUM_BUFFERS + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &phys);
    if (rc < 0) {
        log_printf("sceKernelAllocateDirectMemory a echoue : %x", rc);
        return false;
    }

    void *mem = NULL;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align);
    if (rc < 0) {
        log_printf("sceKernelMapDirectMemory a echoue : %x", rc);
        return false;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buffers[i] = (uint32_t *)((uint8_t *)mem + (size_t)FB_SIZE * i);
        memset(g_buffers[i], 0, FB_SIZE);
    }

    OrbisVideoOutBufferAttribute attr;
    // 0x80000000 = A8R8G8B8 sRGB, mode lineaire
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, SCREEN_W, SCREEN_H, SCREEN_W);
    rc = sceVideoOutRegisterBuffers(g_video, 0, (void **)g_buffers, NUM_BUFFERS, &attr);
    if (rc != 0) {
        log_printf("sceVideoOutRegisterBuffers a echoue : %x", rc);
        return false;
    }

    sceVideoOutSetFlipRate(g_video, 0);
    g_current = 0;
    return true;
}

uint32_t *gfx_backbuffer(void)
{
    return g_buffers[g_current];
}

void gfx_flip(void)
{
    g_frame_id++;
    sceVideoOutSubmitFlip(g_video, g_current, ORBIS_VIDEO_OUT_FLIP_VSYNC, g_frame_id);

    // attendre que ce buffer soit affiche : l'autre devient libre
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg >= g_frame_id)
            break;
        OrbisKernelEvent evt;
        int count;
        if (sceKernelWaitEqueue(g_flip_queue, &evt, 1, &count, 0) != 0)
            break;
    }

    g_current = (g_current + 1) % NUM_BUFFERS;
}

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
    // stb_easy_font n'est pas reentrant avec ce buffer statique : uniquement depuis le thread principal
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
