#pragma once
#include <stdint.h>
#include <stdbool.h>

bool video_start(void);

// True if a frame was decoded since the last video_render()
bool video_has_new_frame(void);

// Converts the newest decoded frame into fb (1920x1080 BGRA) and returns true.
// With force, redraws the previous frame when there is no new one.
bool video_render(uint32_t *fb, bool fullscreen, bool smooth, bool force);

// Forces a reconnect (closes the current socket)
void video_kick(void);
