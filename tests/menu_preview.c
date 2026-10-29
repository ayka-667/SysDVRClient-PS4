// Renders the settings menu natively to PPM files, to check the layout without a PS4.
//   make preview  ->  build/menu_*.ppm
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AppShared g_app;
void config_save(const Config *c) { (void)c; }

static void save(const uint32_t *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
        uint8_t rgb[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

// stand-in for a game frame behind the menu
static void backdrop(uint32_t *fb)
{
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            fb[(size_t)y * SCREEN_W + x] = RGB(40 + x * 60 / SCREEN_W, 70 + y * 80 / SCREEN_H, 120);
}

static void shot(uint32_t *fb, const char *path)
{
    backdrop(fb);
    menu_draw(fb);
    save(fb, path);
}

int main(void)
{
    pthread_mutex_init(&g_app.lock, NULL);
    Config *c = &g_app.cfg;
    c->threads = 2; c->scaler = SCALER_SMOOTH; c->max_delay_ms = 150; c->audio_buffer_ms = 60;
    c->audio = true; c->fullscreen = true;
    strcpy(g_app.target_ip, "192.168.1.42");

    uint32_t *fb = malloc((size_t)SCREEN_W * SCREEN_H * 4);
    menu_open();
    shot(fb, "menu_wifi.ppm");
    menu_input(KEY_DOWN); menu_input(KEY_DOWN); menu_input(KEY_DOWN);
    shot(fb, "menu_performance.ppm");
    menu_input(KEY_UP); menu_input(KEY_UP); menu_input(KEY_UP);
    menu_input(KEY_RIGHT); // connection -> USB
    shot(fb, "menu_usb.ppm");
    menu_input(KEY_LEFT);  // back to Wi-Fi
    menu_input(KEY_DOWN); menu_input(KEY_CONFIRM); // open the IP editor
    shot(fb, "menu_ip.ppm");
    return 0;
}
