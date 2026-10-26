// Native (Linux) harness: runs the real video.c / audio.c / sysdvr_proto.c against fake_switch.py.
// PS4 APIs (log, time, framebuffer, audio) are replaced by stubs.
// SYSDVR_TEST_DECODE_DELAY_MS=N makes every decode N ms slower, to simulate a decoder that cannot keep up.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/video.h"
#include "../src/audio.h"
#include "../src/sysdvr_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

AppShared g_app;
volatile uint64_t g_stub_audio_nonzero_chunks;
int g_test_decode_delay_ms;

uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}
void sleep_ms(int ms) { usleep(ms * 1000); }
void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void app_set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_app.message, sizeof(g_app.message), fmt, ap);
    va_end(ap);
    log_printf("%s", g_app.message);
}
void gfx_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            fb[(size_t)j * SCREEN_W + i] = color;
}

int main(void)
{
    const char *d = getenv("SYSDVR_TEST_DECODE_DELAY_MS");
    g_test_decode_delay_ms = d ? atoi(d) : 0;
    bool slow = g_test_decode_delay_ms > 0;

    pthread_mutex_init(&g_app.lock, NULL);
    // same values as config_defaults() (util.c depends on libkernel)
    g_app.cfg.threads = 2;
    g_app.cfg.audio = true;
    g_app.cfg.fullscreen = true;
    g_app.cfg.smooth = true;
    g_app.cfg.audio_batch = 0;
    g_app.cfg.audio_buffer_ms = 60;
    g_app.cfg.max_delay_ms = 150;

    char ip[64] = { 0 }, serial[64] = { 0 };
    if (!sdvr_discover(5000, ip, sizeof(ip), serial, sizeof(serial))) {
        printf("FAIL: UDP discovery\n");
        return 1;
    }
    printf("Discovery OK: %s serial=%s\n", ip, serial);
    strcpy(g_app.target_ip, ip);

    if (!video_start() || !audio_start()) {
        printf("FAIL: could not start the threads\n");
        return 1;
    }

    uint32_t *fb = malloc((size_t)SCREEN_W * SCREEN_H * 4);
    int rendered = 0;
    int32_t max_delay = 0;
    uint64_t delay_sum = 0, delay_n = 0;
    uint64_t t0 = now_us();
    while (now_us() - t0 < 8000000) {
        if (video_has_new_frame() && video_render(fb, true, true, false)) {
            rendered++;
            // ignore the first second (connection, first keyframe)
            if (now_us() - t0 > 1000000) {
                if (g_app.video_delay_ms > max_delay)
                    max_delay = g_app.video_delay_ms;
                delay_sum += g_app.video_delay_ms > 0 ? g_app.video_delay_ms : 0;
                delay_n++;
            }
        } else {
            usleep(1000);
        }
    }

    // one frame in PPM for a visual check
    FILE *f = fopen("frame.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
        uint32_t p = fb[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);

    int avg_delay = delay_n ? (int)(delay_sum / delay_n) : -1;
    printf("Decode delay=%d ms  frames decoded=%u rendered=%d skipped=%u  video bytes=%llu  delay avg=%d max=%d ms\n",
           g_test_decode_delay_ms, g_app.video_frames, rendered, g_app.video_dropped, (unsigned long long)g_app.video_bytes,
           avg_delay, max_delay);
    printf("Decode %.1f ms/frame  audio chunks=%llu fill=%u ms underruns=%u  state video=%d audio=%d\n",
           g_app.decode_us / 1000.0, (unsigned long long)g_stub_audio_nonzero_chunks, g_app.audio_fill_ms,
           g_app.audio_underruns, g_app.video_state, g_app.audio_state);

    bool ok;
    if (!slow) {
        // real time: nothing dropped, low delay
        ok = g_app.video_frames > 200 && rendered > 180 && g_app.video_dropped == 0 && max_delay < 150 &&
             g_stub_audio_nonzero_chunks > 1000;
    } else {
        // decoder slower than the stream: must skip frames and keep the delay bounded instead of growing
        ok = g_app.video_dropped > 0 && rendered > 30 && max_delay < g_app.cfg.max_delay_ms + 250;
    }
    printf(ok ? "RESULT: OK\n" : "RESULT: FAIL\n");
    g_app.quit = true;
    return ok ? 0 : 1;
}
