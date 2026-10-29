#pragma once
#include <stdbool.h>

// USB mode: the Switch (SysDVR set to USB) plugged into the PS4. Video and audio come multiplexed over one bulk endpoint.
// The thread does nothing while the connection setting is "network".
bool usb_start(void);
