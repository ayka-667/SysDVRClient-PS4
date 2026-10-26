// Stub natif de sceAudioOut pour tester audio.c sur PC
#pragma once
#include <stdint.h>
#include <unistd.h>
#include <string.h>

enum OrbisAudioOutPort { ORBIS_AUDIO_OUT_PORT_TYPE_MAIN = 0 };
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1

extern volatile uint64_t g_stub_audio_nonzero_chunks;

static inline int32_t sceAudioOutInit(void) { return 0; }
static inline int32_t sceAudioOutOpen(int32_t u, enum OrbisAudioOutPort p, int32_t i, uint32_t len, uint32_t rate, uint32_t fmt)
{
    (void)u; (void)p; (void)i; (void)len; (void)rate; (void)fmt;
    return 1;
}
static inline int32_t sceAudioOutOutput(int32_t h, const void *data)
{
    (void)h;
    if (data) {
        const uint8_t *d = (const uint8_t *)data;
        for (int i = 0; i < 1024; i++)
            if (d[i]) { g_stub_audio_nonzero_chunks++; break; }
    }
    usleep(5333);
    return 0;
}
static inline int32_t sceAudioOutClose(int32_t h) { (void)h; return 0; }
