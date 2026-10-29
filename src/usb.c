// USB transport. SysDVR in USB mode shows up as VID 18D1 / PID 4EE0 with one interface and a pair of bulk endpoints.
// Same handshake as TCP (hello -> request -> response), then every bulk transfer carries one whole packet
// (18-byte header + payload), video and audio mixed. The Switch drops the client if it cannot send for 1.5 s,
// so this thread only reads and dispatches; decoding happens on the video threads.
#include "usb.h"
#include "common.h"
#include "video.h"
#include "audio.h"
#include "sysdvr_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>
#include <orbis/Usbd.h>
#include <orbis/Sysmodule.h>

#define USB_TIMEOUT_MS   1000
#define USB_SMALL_READ   512
// one transfer holds a whole packet, rounded up to the USB 2.0 packet size
#define USB_BUF_SIZE     ((SDVR_MAX_PAYLOAD + sizeof(SdvrPacketHeader) + 511) & ~511u)

static pthread_t g_thread;

typedef struct {
    libusb_device_handle *handle;
    int iface;
    unsigned char ep_in, ep_out;
} UsbDev;

// sceUsbd returns either libusb codes or SCE codes built from them (0x8024xxxx)
static bool is_timeout(int rc)
{
    return rc == -7 || (uint32_t)rc == 0x80240007u;
}

static void set_states(LinkState s)
{
    pthread_mutex_lock(&g_app.lock);
    g_app.video_state = s;
    g_app.audio_state = g_app.cfg.audio ? s : LINK_IDLE;
    pthread_mutex_unlock(&g_app.lock);
}

static void dev_close(UsbDev *d)
{
    if (!d->handle)
        return;
    sceUsbdReleaseInterface(d->handle, d->iface);
    sceUsbdClose(d->handle);
    d->handle = NULL;
}

static void find_endpoints(UsbDev *d)
{
    // SysDVR's defaults, used if the descriptor can't be read
    d->iface = 0;
    d->ep_in = 0x81;
    d->ep_out = 0x01;

    struct libusb_config_descriptor *cfg = NULL;
    if (sceUsbdGetActiveConfigDescriptor(sceUsbdGetDevice(d->handle), &cfg) < 0 || !cfg)
        return;
    if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
        const struct libusb_interface_descriptor *alt = &cfg->interface[0].altsetting[0];
        d->iface = alt->bInterfaceNumber;
        for (int i = 0; i < alt->bNumEndpoints; i++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[i];
            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                d->ep_in = ep->bEndpointAddress;
            else
                d->ep_out = ep->bEndpointAddress;
        }
    }
    sceUsbdFreeConfigDescriptor(cfg);
}

static bool dev_open(UsbDev *d, char *serial, size_t seriallen)
{
    d->handle = sceUsbdOpenDeviceWithVidPid(SDVR_USB_VID, SDVR_USB_PID);
    if (!d->handle)
        return false;

    // the serial is the SysDVR descriptor "SysDVR|<version>|<proto>|<serial>"; an Android phone in fastboot uses the same IDs
    struct libusb_device_descriptor desc;
    unsigned char str[128] = { 0 };
    if (sceUsbdGetDeviceDescriptor(sceUsbdGetDevice(d->handle), &desc) >= 0 && desc.iSerialNumber &&
        sceUsbdGetStringDescriptorAscii(d->handle, desc.iSerialNumber, 0x0409, str, sizeof(str) - 1) > 0) {
        if (strncmp((char *)str, "SysDVR|", 7) != 0) {
            log_printf("USB device 18D1:4EE0 is not a SysDVR Switch (%s)", (char *)str);
            sceUsbdClose(d->handle);
            d->handle = NULL;
            return false;
        }
        const char *last = strrchr((char *)str, '|');
        snprintf(serial, seriallen, "%s", last ? last + 1 : "");
        log_printf("USB device: %s", (char *)str);
    }

    find_endpoints(d);
    int rc = sceUsbdClaimInterface(d->handle, d->iface);
    if (rc < 0) {
        log_printf("sceUsbdClaimInterface failed: %x", rc);
        sceUsbdClose(d->handle);
        d->handle = NULL;
        return false;
    }
    log_printf("USB endpoints: in %02x out %02x (interface %d)", d->ep_in, d->ep_out, d->iface);
    return true;
}

static int bulk_read(UsbDev *d, uint8_t *buf, int len, int *got, int timeout_ms)
{
    *got = 0;
    return sceUsbdBulkTransfer(d->handle, d->ep_in, buf, len, got, (uint32_t)timeout_ms);
}

// hello may already have been read (got_hello bytes in buf); otherwise waits for it. Returns 0 on success.
static int handshake(UsbDev *d, uint8_t *buf, int got_hello, const Config *cfg, char *err, size_t errlen)
{
    int got = got_hello;
    // the Switch rewrites the hello every second until someone reads it
    for (int tries = 0; got == 0 && tries < 3; tries++) {
        int rc = bulk_read(d, buf, USB_SMALL_READ, &got, 1500);
        if (rc < 0 && !is_timeout(rc)) {
            snprintf(err, errlen, "USB read failed (%x)", rc);
            return -1;
        }
    }
    if (got == 0) {
        snprintf(err, errlen, "The Switch did not send the SysDVR hello");
        return -1;
    }

    uint16_t ver;
    if (sdvr_parse_hello(buf, (size_t)got, "USB", &ver, err, errlen) != 0)
        return -1;

    SdvrHandshakeRequest req;
    SdvrHandshakeOptions opt = { .video = true, .audio = cfg->audio, .audio_batching = cfg->audio_batch, .turn_off_screen = cfg->screen_off };
    sdvr_build_request(&req, ver, &opt);
    int sent = 0;
    int rc = sceUsbdBulkTransfer(d->handle, d->ep_out, (unsigned char *)&req, sizeof(req), &sent, 1500);
    if (rc < 0 || sent != (int)sizeof(req)) {
        snprintf(err, errlen, "Could not send the handshake over USB (%x)", rc);
        return -1;
    }

    rc = bulk_read(d, buf, USB_SMALL_READ, &got, 1500);
    if (rc < 0 || got < (int)sdvr_response_size(ver)) {
        snprintf(err, errlen, "No handshake response over USB (%x, %d bytes)", rc, got);
        return -1;
    }
    return sdvr_check_response(buf, (size_t)got, err, errlen);
}

static bool is_hello(const uint8_t *buf, int got)
{
    return got == SDVR_HELLO_SIZE && memcmp(buf, "SysDVR|", 7) == 0;
}

static void *usb_thread(void *arg)
{
    (void)arg;
    bool inited = false;
    bool waiting_logged = false;
    uint8_t *buf = malloc(USB_BUF_SIZE);
    UsbDev dev = { 0 };
    char err[256];

    while (!g_app.quit) {
        Config cfg;
        pthread_mutex_lock(&g_app.lock);
        cfg = g_app.cfg;
        int gen = g_app.generation;
        pthread_mutex_unlock(&g_app.lock);

        if (!cfg.usb) {
            g_app.usb_connected = false;
            waiting_logged = false;
            sleep_ms(200);
            continue;
        }

        if (!inited) {
            int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
            if (rc >= 0)
                rc = sceUsbdInit();
            if (rc < 0) {
                app_set_message("USB is not available (%x)", rc);
                set_states(LINK_ERROR);
                sleep_ms(3000);
                continue;
            }
            inited = true;
        }

        set_states(LINK_CONNECTING);
        char serial[64] = { 0 };
        if (!dev_open(&dev, serial, sizeof(serial))) {
            g_app.usb_connected = false;
            if (!waiting_logged) {
                app_set_message("Waiting for the Switch on USB (SysDVR in USB mode)");
                waiting_logged = true;
            }
            sleep_ms(1000);
            continue;
        }
        g_app.usb_connected = true;
        waiting_logged = false;
        pthread_mutex_lock(&g_app.lock);
        snprintf(g_app.serial, sizeof(g_app.serial), "%s", serial);
        pthread_mutex_unlock(&g_app.lock);

        int pending_hello = 0;
        while (!g_app.quit && g_app.generation == gen && g_app.cfg.usb) {
            if (handshake(&dev, buf, pending_hello, &cfg, err, sizeof(err)) != 0) {
                app_set_message("USB: %s", err);
                set_states(LINK_ERROR);
                break;
            }
            pending_hello = 0;
            video_usb_begin();
            audio_usb_begin();
            set_states(LINK_STREAMING);
            app_set_message("Connected to the Switch over USB");

            for (;;) {
                if (g_app.quit || g_app.generation != gen || !g_app.cfg.usb)
                    break;
                int got = 0;
                int rc = bulk_read(&dev, buf, USB_BUF_SIZE, &got, USB_TIMEOUT_MS);
                if (rc < 0 && is_timeout(rc) && got == 0)
                    continue; // nothing to send (no game running?)
                if (rc < 0) {
                    app_set_message("USB: connection lost (%x)", rc);
                    break;
                }
                if (is_hello(buf, got)) {
                    // the Switch timed out on us and restarted the handshake
                    log_printf("USB: Switch asked for a new handshake");
                    pending_hello = got;
                    break;
                }
                if (got < (int)sizeof(SdvrPacketHeader))
                    continue;

                SdvrPacketHeader hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                if (!sdvr_header_valid(&hdr) || got < (int)(sizeof(hdr) + hdr.DataSize)) {
                    log_printf("USB: bad packet (%d bytes, magic %08x, size %u)", got, hdr.Magic, hdr.DataSize);
                    continue;
                }
                const uint8_t *payload = buf + sizeof(hdr);
                if (hdr.MetaData & SDVR_META_AUDIO) {
                    if (g_app.cfg.audio && !(hdr.MetaData & SDVR_META_ERROR))
                        audio_push_pcm(payload, hdr.DataSize);
                } else {
                    video_usb_packet(&hdr, payload);
                }
            }
            if (!pending_hello)
                break;
        }

        dev_close(&dev);
        g_app.usb_connected = false;
        set_states(LINK_CONNECTING);
        sleep_ms(500);
    }

    dev_close(&dev);
    free(buf);
    return NULL;
}

bool usb_start(void)
{
    return pthread_create(&g_thread, NULL, usb_thread, NULL) == 0;
}
