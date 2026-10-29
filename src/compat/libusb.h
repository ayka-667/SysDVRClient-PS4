#pragma once
// orbis/Usbd.h includes <libusb.h>: the OpenOrbis definitions live in orbis/_types and lack the endpoint descriptor,
// which is defined here with the standard libusb layout (sceUsbd is a libusb port).
#include <orbis/_types/libusb.h>

struct libusb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
    uint8_t  bRefresh;
    uint8_t  bSynchAddress;
    const unsigned char *extra;
    int32_t  extra_length;
};

#define LIBUSB_ENDPOINT_IN          0x80
#define LIBUSB_TRANSFER_TYPE_MASK   0x03
#define LIBUSB_TRANSFER_TYPE_BULK   2
