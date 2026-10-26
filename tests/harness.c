// Harness natif (Linux) : fait tourner les vrais video.c / audio.c / sysdvr_proto.c contre fake_switch.py.
// Les APIs PS4 (log, temps, framebuffer, audio) sont remplacees par des stubs.
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
    pthread_mutex_init(&g_app.lock, NULL);
    // memes valeurs que config_defaults() (util.c depend de libkernel)
    g_app.cfg.threads = 3;
    g_app.cfg.audio = true;
    g_app.cfg.fullscreen = true;
    g_app.cfg.audio_batch = 2;
    g_app.cfg.audio_buffer_ms = 80;

    char ip[64] = { 0 }, serial[64] = { 0 };
    if (!sdvr_discover(5000, ip, sizeof(ip), serial, sizeof(serial))) {
        printf("ECHEC : decouverte UDP\n");
        return 1;
    }
    printf("Decouverte OK : %s serie=%s\n", ip, serial);
    strcpy(g_app.target_ip, ip);

    if (!video_start() || !audio_start()) {
        printf("ECHEC : demarrage des threads\n");
        return 1;
    }

    uint32_t *fb = malloc((size_t)SCREEN_W * SCREEN_H * 4);
    int rendered = 0;
    uint64_t t0 = now_us();
    while (now_us() - t0 < 6000000) {
        if (video_render(fb, true, false))
            rendered++;
        else
            usleep(2000);
    }

    // une image du milieu en PPM pour verification visuelle
    FILE *f = fopen("frame.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
        uint32_t p = fb[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);

    printf("Images decodees=%u rendues=%d octets video=%llu chunks audio non vides=%llu etat video=%d audio=%d\n",
           g_app.video_frames, rendered, (unsigned long long)g_app.video_bytes,
           (unsigned long long)g_stub_audio_nonzero_chunks, g_app.video_state, g_app.audio_state);
    bool ok = g_app.video_frames > 120 && rendered > 100 && g_stub_audio_nonzero_chunks > 500;
    printf(ok ? "RESULTAT : OK\n" : "RESULTAT : ECHEC\n");
    g_app.quit = true;
    return ok ? 0 : 1;
}
