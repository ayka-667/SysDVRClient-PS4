#pragma once
#include <stdbool.h>
#include <stdint.h>

bool audio_start(void);
void audio_kick(void);

// USB mode: audio arrives multiplexed with the video on the USB thread
void audio_usb_begin(void);
void audio_push_pcm(const uint8_t *data, uint32_t len);
