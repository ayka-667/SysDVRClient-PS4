#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

// Colours in framebuffer format (B8G8R8A8 in memory, i.e. 0xAARRGGBB)
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

bool gfx_init(void);

// Buffer to draw the next frame into
uint32_t *gfx_backbuffer(void);
// Queues the backbuffer for the next vsync and waits until the next buffer is free
void gfx_flip(void);

void gfx_clear(uint32_t *fb, uint32_t color);
void gfx_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color);
// Draws text (stb_easy_font vector font), scale = size of one font "pixel"
void gfx_text(uint32_t *fb, int x, int y, int scale, uint32_t color, const char *text);
int  gfx_text_width(const char *text, int scale);
void gfx_text_centered(uint32_t *fb, int y, int scale, uint32_t color, const char *text);
