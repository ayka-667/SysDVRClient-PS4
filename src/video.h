#pragma once
#include <stdint.h>
#include <stdbool.h>

bool video_start(void);

// Si une nouvelle image est disponible, la convertit dans fb (1920x1080 BGRA) et retourne true.
bool video_render(uint32_t *fb, bool fullscreen, bool fast);

// Force la reconnexion (fermeture du socket en cours)
void video_kick(void);
