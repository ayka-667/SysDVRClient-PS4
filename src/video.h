#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "sysdvr_proto.h"

bool video_start(void);

// True if a frame was decoded since the last video_render()
bool video_has_new_frame(void);

// Converts the newest decoded frame into fb (1920x1080 BGRA) and returns true.
// With force, redraws the previous frame when there is no new one.
// scaler is one of SCALER_FAST / SCALER_SMOOTH / SCALER_SHARP (common.h)
bool video_render(uint32_t *fb, bool fullscreen, int scaler, bool force);

// Forces a reconnect (closes the current socket)
void video_kick(void);

// USB mode: called by the USB thread for every packet
void video_usb_begin(void);
void video_usb_packet(const SdvrPacketHeader *hdr, const uint8_t *data);
