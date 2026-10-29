#pragma once
// Client implementation of the SysDVR "TCP Bridge" protocol (protocol 02 / 03).
// Reference : https://github.com/exelix11/SysDVR/blob/master/protocol.md
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SDVR_PORT_VIDEO     9911
#define SDVR_PORT_AUDIO     9922
#define SDVR_PORT_DISCOVERY 19999

#define SDVR_PACKET_MAGIC   0xCCCCCCCCu
#define SDVR_REQUEST_MAGIC  0xAAAAAAAAu
#define SDVR_HANDSHAKE_OK   6
#define SDVR_HELLO_SIZE     10

// Max packet payload: max(VbufSz, AbufSz * (1 + MaxABatching))
#define SDVR_VBUF_SIZE      0x54000
#define SDVR_ABUF_SIZE      0x1000
#define SDVR_MAX_PAYLOAD    SDVR_VBUF_SIZE

// USB: the Switch shows up as VID 18D1 / PID 4EE0 and sends one packet per bulk transfer
#define SDVR_USB_VID        0x18D1
#define SDVR_USB_PID        0x4EE0

enum {
    SDVR_META_VIDEO    = 1 << 0,
    SDVR_META_AUDIO    = 1 << 1,
    SDVR_META_DATA     = 1 << 2,
    SDVR_META_REPLAY   = 1 << 3,
    SDVR_META_MULTINAL = 1 << 4,
    SDVR_META_ERROR    = 1 << 5,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t Magic;
    uint32_t DataSize;
    uint64_t Timestamp; // microseconds
    uint8_t  MetaData;
    uint8_t  ReplaySlot;
} SdvrPacketHeader;

typedef struct {
    uint32_t Magic;
    uint16_t ProtoVer;     // two ASCII characters, e.g. '0','2'
    uint8_t  MetaFlags;    // 1 = video, 2 = audio
    uint8_t  VideoFlags;   // 1 = NalHash, 2 = InjectPPSSPS, 4 = NalHashOnlyIDR
    uint8_t  AudioBatching;
    uint8_t  FeatureFlags; // 1 = turn off the screen (proto 03)
    uint8_t  Reserved[6];
} SdvrHandshakeRequest;
#pragma pack(pop)

_Static_assert(sizeof(SdvrPacketHeader) == 18, "header size");
_Static_assert(sizeof(SdvrHandshakeRequest) == 16, "handshake size");

typedef struct {
    bool video;
    bool audio;
    int  audio_batching;
    bool turn_off_screen;
} SdvrHandshakeOptions;

// Default SPS/PPS of the Switch stream (same as the official client)
extern const uint8_t SDVR_SPS[20];
extern const uint8_t SDVR_PPS[8];

// TCP connect with timeout. Returns the fd or -1.
int sdvr_connect(const char *ip, int port, int timeout_ms, char *err, size_t errlen);

// Handshake building blocks, shared by TCP and USB.
// Checks the 10-byte hello; mode is only used in the error message. Returns 0 on success.
int sdvr_parse_hello(const uint8_t *hello, size_t len, const char *mode, uint16_t *proto_ver, char *err, size_t errlen);
void sdvr_build_request(SdvrHandshakeRequest *req, uint16_t proto_ver, const SdvrHandshakeOptions *opt);
size_t sdvr_response_size(uint16_t proto_ver);
int sdvr_check_response(const uint8_t *resp, size_t len, char *err, size_t errlen);
bool sdvr_header_valid(const SdvrPacketHeader *hdr);

// Performs the whole handshake on a TCP socket. Returns 0 on success.
int sdvr_handshake(int fd, const SdvrHandshakeOptions *opt, char *err, size_t errlen);

// Reads the next packet header (resyncs on garbage).
// Returns 1 if a header was read, 0 on timeout (no data), -1 on error/disconnect.
// *stop is checked on every timeout so the read can be interrupted.
int sdvr_read_header(int fd, SdvrPacketHeader *hdr, volatile bool *stop);

// Reads exactly size payload bytes. Returns 1 on success, -1 on error.
int sdvr_read_payload(int fd, uint8_t *buf, uint32_t size, volatile bool *stop);

// Header + payload. buf must hold at least SDVR_MAX_PAYLOAD bytes. Same return values as sdvr_read_header.
int sdvr_read_packet(int fd, SdvrPacketHeader *hdr, uint8_t *buf, volatile bool *stop);

// Listens for the Switch's UDP announcements. Returns 1 if found (ip/serial filled in), 0 otherwise.
int sdvr_discover(int timeout_ms, char *ip, size_t iplen, char *serial, size_t seriallen);

// Formats an error packet sent by the console
void sdvr_format_error(const uint8_t *data, uint32_t size, char *out, size_t outlen);

void sdvr_close(int fd);
