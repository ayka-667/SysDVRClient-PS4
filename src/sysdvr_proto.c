#include "sysdvr_proto.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

const uint8_t SDVR_SPS[20] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
const uint8_t SDVR_PPS[8]  = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

static void set_timeout(int fd, int optname, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

void sdvr_close(int fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

int sdvr_connect(const char *ip, int port, int timeout_ms, char *err, size_t errlen)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        snprintf(err, errlen, "Invalid IP address: %s", ip);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // non-blocking connect so a timeout can be applied
    int flags = fcntl(fd, F_GETFL, 0);
    bool nonblock = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && nonblock && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            snprintf(err, errlen, "No answer from %s:%d", ip, port);
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            snprintf(err, errlen, "Connection to %s:%d refused (%s)", ip, port, strerror(soerr));
            close(fd);
            return -1;
        }
        rc = 0;
    }

    if (rc != 0) {
        snprintf(err, errlen, "Cannot connect to %s:%d (%s)", ip, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (nonblock)
        fcntl(fd, F_SETFL, flags);

    set_timeout(fd, SO_RCVTIMEO, 1000);
    set_timeout(fd, SO_SNDTIMEO, 3000);
    return fd;
}

// Reads exactly len bytes. Returns 1 on success, 0 on timeout with nothing read (if allow_idle), -1 on error.
static int read_full(int fd, void *dst, size_t len, volatile bool *stop, bool allow_idle, int max_idle_s)
{
    uint8_t *p = (uint8_t *)dst;
    size_t got = 0;
    int idle = 0;

    while (got < len) {
        if (stop && *stop)
            return -1;

        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
            idle = 0;
            continue;
        }
        if (n == 0)
            return -1; // connection closed

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ETIMEDOUT) {
            if (got == 0 && allow_idle)
                return 0;
            if (++idle > max_idle_s)
                return -1;
            continue;
        }
        return -1;
    }
    return 1;
}

int sdvr_handshake(int fd, const SdvrHandshakeOptions *opt, char *err, size_t errlen)
{
    char hello[10];
    if (read_full(fd, hello, sizeof(hello), NULL, false, 5) != 1) {
        snprintf(err, errlen, "The Switch did not send the SysDVR hello");
        return -1;
    }

    if (memcmp(hello, "SysDVR|", 7) != 0 || hello[9] != 0) {
        snprintf(err, errlen, "Unexpected reply: is SysDVR in TCP Bridge mode?");
        return -1;
    }

    char v0 = hello[7], v1 = hello[8];
    bool v2 = v0 == '0' && v1 == '2';
    bool v3 = v0 == '0' && v1 == '3';
    if (!v2 && !v3) {
        snprintf(err, errlen, "Unsupported SysDVR protocol version (%c%c). Update SysDVR.", v0, v1);
        return -1;
    }

    SdvrHandshakeRequest req;
    memset(&req, 0, sizeof(req));
    req.Magic = SDVR_REQUEST_MAGIC;
    req.ProtoVer = (uint16_t)((uint8_t)v0 | ((uint8_t)v1 << 8));
    req.MetaFlags = opt->video ? 1 : 2;
    req.VideoFlags = 2; // InjectPPSSPS, no NAL replay
    req.AudioBatching = (uint8_t)opt->audio_batching;
    if (v3 && opt->turn_off_screen)
        req.FeatureFlags |= 1;

    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
        snprintf(err, errlen, "Could not send the handshake");
        return -1;
    }

    uint8_t resp[72];
    size_t resp_len = v3 ? 72 : 4;
    if (read_full(fd, resp, resp_len, NULL, false, 5) != 1) {
        snprintf(err, errlen, "No handshake response");
        return -1;
    }

    uint32_t code;
    memcpy(&code, resp, 4);
    if (code != SDVR_HANDSHAKE_OK) {
        if (code == 1)
            snprintf(err, errlen, "Version rejected by the Switch: update SysDVR");
        else
            snprintf(err, errlen, "Handshake rejected by the Switch (code %u)", code);
        return -1;
    }
    return 0;
}

int sdvr_read_header(int fd, SdvrPacketHeader *hdr, volatile bool *stop)
{
    int rc = read_full(fd, hdr, sizeof(*hdr), stop, true, 10);
    if (rc != 1)
        return rc;

    // resync if the header is corrupted: look for CC CC CC CC
    int resync_budget = 4 * 1024 * 1024;
    while (hdr->Magic != SDVR_PACKET_MAGIC || hdr->DataSize > SDVR_MAX_PAYLOAD) {
        if (--resync_budget <= 0)
            return -1;
        memmove(hdr, ((uint8_t *)hdr) + 1, sizeof(*hdr) - 1);
        rc = read_full(fd, ((uint8_t *)hdr) + sizeof(*hdr) - 1, 1, stop, false, 10);
        if (rc != 1)
            return -1;
    }
    return 1;
}

int sdvr_read_payload(int fd, uint8_t *buf, uint32_t size, volatile bool *stop)
{
    if (size == 0)
        return 1;
    return read_full(fd, buf, size, stop, false, 10) == 1 ? 1 : -1;
}

int sdvr_read_packet(int fd, SdvrPacketHeader *hdr, uint8_t *buf, volatile bool *stop)
{
    int rc = sdvr_read_header(fd, hdr, stop);
    if (rc != 1)
        return rc;
    return sdvr_read_payload(fd, buf, hdr->DataSize, stop);
}

int sdvr_discover(int timeout_ms, char *ip, size_t iplen, char *serial, size_t seriallen)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SDVR_PORT_DISCOVERY);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }

    set_timeout(fd, SO_RCVTIMEO, timeout_ms);

    int found = 0;
    char pkt[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, pkt, sizeof(pkt) - 1, 0, (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        pkt[n] = 0; // the string ends at the first NUL
        // format: SysDVR|<version>|<proto>|<serial>
        if (strncmp(pkt, "SysDVR|", 7) == 0) {
            char *ver = pkt + 7;
            char *proto = strchr(ver, '|');
            if (proto) {
                proto++;
                char *ser = strchr(proto, '|');
                if (ser) {
                    ser++;
                    inet_ntop(AF_INET, &from.sin_addr, ip, (socklen_t)iplen);
                    snprintf(serial, seriallen, "%s", ser);
                    found = 1;
                }
            }
        }
    }
    close(fd);
    return found;
}

void sdvr_format_error(const uint8_t *data, uint32_t size, char *out, size_t outlen)
{
    if (size < 8) {
        snprintf(out, outlen, "Unknown console error");
        return;
    }
    uint32_t type, code;
    uint64_t ctx1 = 0;
    memcpy(&type, data, 4);
    memcpy(&code, data + 4, 4);
    if (size >= 16)
        memcpy(&ctx1, data + 8, 8);

    switch (type) {
    case 1: snprintf(out, outlen, "Video capture failed (0x%x)", code); break;
    case 2:
    case 3: snprintf(out, outlen, "Audio capture failed (0x%x)", code); break;
    case 4: snprintf(out, outlen, "grc:d (video) failed (0x%x)", code); break;
    case 5: snprintf(out, outlen, "grc:d (audio) failed (0x%x)", code); break;
    default: snprintf(out, outlen, "Console error 0x%x / 0x%x", type, code); break;
    }
    (void)ctx1;
}
