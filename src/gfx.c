#include "gfx.h"
#include "common.h"

#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

// triple buffering: drawing never waits for the frame currently being flipped
#define NUM_BUFFERS 3
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
        log_printf("sceVideoOutOpen failed: %x", g_video);
        return false;
    }

    if (sceKernelCreateEqueue(&g_flip_queue, "sysdvr flip queue") < 0) {
        log_printf("sceKernelCreateEqueue failed");
        return false;
    }
    sceVideoOutAddFlipEvent(g_flip_queue, g_video, 0);

    const size_t align = 0x200000;
    size_t total = ((size_t)FB_SIZE * NUM_BUFFERS + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &phys);
    if (rc < 0) {
        log_printf("sceKernelAllocateDirectMemory failed: %x", rc);
        return false;
    }

    void *mem = NULL;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align);
    if (rc < 0) {
        log_printf("sceKernelMapDirectMemory failed: %x", rc);
        return false;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buffers[i] = (uint32_t *)((uint8_t *)mem + (size_t)FB_SIZE * i);
        memset(g_buffers[i], 0, FB_SIZE);
    }

    OrbisVideoOutBufferAttribute attr;
    // 0x80000000 = A8R8G8B8 sRGB, linear tiling
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, SCREEN_W, SCREEN_H, SCREEN_W);
    rc = sceVideoOutRegisterBuffers(g_video, 0, (void **)g_buffers, NUM_BUFFERS, &attr);
    if (rc != 0) {
        log_printf("sceVideoOutRegisterBuffers failed: %x", rc);
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

    // the next buffer was last shown NUM_BUFFERS - 1 flips ago: it is free once the previous flip has happened
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg >= g_frame_id - (NUM_BUFFERS - 2))
            break;
        OrbisKernelEvent evt;
        int count;
        if (sceKernelWaitEqueue(g_flip_queue, &evt, 1, &count, 0) != 0)
            break;
    }

    g_current = (g_current + 1) % NUM_BUFFERS;
}
