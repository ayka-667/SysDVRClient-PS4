#include "audio.h"
#include "common.h"
#include "sysdvr_proto.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

// La Switch envoie du PCM 16 bits stereo 48 kHz
#define SAMPLE_RATE      48000
#define BYTES_PER_FRAME  4
#define OUT_GRAIN        256
#define OUT_CHUNK        (OUT_GRAIN * BYTES_PER_FRAME)
#define RING_SIZE        (SAMPLE_RATE * BYTES_PER_FRAME) // 1 seconde

static pthread_t g_net_thread, g_out_thread;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_ring[RING_SIZE];
static size_t g_ring_read, g_ring_fill;
static volatile int g_sock = -1;

void audio_kick(void)
{
    int fd = g_sock;
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

static size_t ms_to_bytes(int ms)
{
    return (size_t)SAMPLE_RATE * ms / 1000 * BYTES_PER_FRAME;
}

static void ring_push(const uint8_t *data, size_t len, size_t max_fill)
{
    pthread_mutex_lock(&g_ring_lock);
    if (len > RING_SIZE) {
        data += len - RING_SIZE;
        len = RING_SIZE;
    }
    for (size_t i = 0; i < len;) {
        size_t w = (g_ring_read + g_ring_fill) % RING_SIZE;
        size_t n = RING_SIZE - w;
        if (n > len - i) n = len - i;
        memcpy(g_ring + w, data + i, n);
        i += n;
        g_ring_fill += n;
        if (g_ring_fill > RING_SIZE) {
            g_ring_read = (g_ring_read + (g_ring_fill - RING_SIZE)) % RING_SIZE;
            g_ring_fill = RING_SIZE;
        }
    }
    // trop de retard accumule : on jette le plus ancien pour garder une latence faible
    if (g_ring_fill > max_fill) {
        size_t drop = (g_ring_fill - max_fill / 2) & ~(size_t)(BYTES_PER_FRAME - 1);
        g_ring_read = (g_ring_read + drop) % RING_SIZE;
        g_ring_fill -= drop;
    }
    pthread_mutex_unlock(&g_ring_lock);
}

static bool ring_pop(uint8_t *out, size_t len, size_t prime)
{
    static bool primed = false;
    bool ok = false;
    pthread_mutex_lock(&g_ring_lock);
    if (!primed && g_ring_fill >= prime)
        primed = true;
    if (primed && g_ring_fill >= len) {
        for (size_t i = 0; i < len;) {
            size_t n = RING_SIZE - g_ring_read;
            if (n > len - i) n = len - i;
            memcpy(out + i, g_ring + g_ring_read, n);
            g_ring_read = (g_ring_read + n) % RING_SIZE;
            i += n;
        }
        g_ring_fill -= len;
        ok = true;
    } else if (primed) {
        primed = false; // sous-alimentation : on attend de remplir a nouveau
    }
    pthread_mutex_unlock(&g_ring_lock);
    return ok;
}

static void ring_reset(void)
{
    pthread_mutex_lock(&g_ring_lock);
    g_ring_read = g_ring_fill = 0;
    pthread_mutex_unlock(&g_ring_lock);
}

static void *audio_out_thread(void *arg)
{
    (void)arg;
    int handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, OUT_GRAIN, SAMPLE_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle <= 0) {
        log_printf("sceAudioOutOpen a echoue : %x", handle);
        return NULL;
    }

    static uint8_t chunk[OUT_CHUNK] __attribute__((aligned(64)));
    while (!g_app.quit) {
        int prime_ms = g_app.cfg.audio_buffer_ms / 2;
        if (!ring_pop(chunk, OUT_CHUNK, ms_to_bytes(prime_ms)))
            memset(chunk, 0, OUT_CHUNK);
        // bloque jusqu'a ce que le materiel soit pret : cadence la lecture
        sceAudioOutOutput(handle, chunk);
    }
    sceAudioOutOutput(handle, NULL);
    sceAudioOutClose(handle);
    return NULL;
}

static void set_state(LinkState s)
{
    pthread_mutex_lock(&g_app.lock);
    g_app.audio_state = s;
    pthread_mutex_unlock(&g_app.lock);
}

static void *audio_net_thread(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(SDVR_MAX_PAYLOAD);
    char err[256];

    while (!g_app.quit) {
        char ip[64];
        Config cfg;
        pthread_mutex_lock(&g_app.lock);
        strcpy(ip, g_app.target_ip);
        cfg = g_app.cfg;
        int gen = g_app.generation;
        pthread_mutex_unlock(&g_app.lock);

        if (!ip[0] || !cfg.audio) {
            set_state(LINK_IDLE);
            sleep_ms(200);
            continue;
        }

        set_state(LINK_CONNECTING);
        int fd = sdvr_connect(ip, SDVR_PORT_AUDIO, 3000, err, sizeof(err));
        if (fd >= 0 && sdvr_handshake(fd, &(SdvrHandshakeOptions){ .video = false, .audio_batching = cfg.audio_batch, .turn_off_screen = cfg.screen_off }, err, sizeof(err)) != 0) {
            sdvr_close(fd);
            fd = -1;
        }
        if (fd < 0) {
            log_printf("Audio : %s", err);
            set_state(LINK_ERROR);
            for (int i = 0; i < 20 && !g_app.quit && g_app.generation == gen; i++)
                sleep_ms(100);
            continue;
        }

        g_sock = fd;
        log_printf("Audio connecte a %s", ip);
        set_state(LINK_STREAMING);
        ring_reset();

        size_t max_fill = ms_to_bytes(cfg.audio_buffer_ms * 2);
        volatile bool stop = false;
        SdvrPacketHeader hdr;
        while (!g_app.quit && g_app.generation == gen && g_app.cfg.audio) {
            int rc = sdvr_read_packet(fd, &hdr, buf, &stop);
            if (rc < 0) {
                log_printf("Audio : connexion perdue");
                break;
            }
            if (rc == 0)
                continue;
            if ((hdr.MetaData & SDVR_META_AUDIO) && !(hdr.MetaData & SDVR_META_ERROR) && hdr.DataSize > 0)
                ring_push(buf, hdr.DataSize & ~(uint32_t)(BYTES_PER_FRAME - 1), max_fill);
        }

        g_sock = -1;
        sdvr_close(fd);
        set_state(LINK_CONNECTING);
    }
    free(buf);
    return NULL;
}

bool audio_start(void)
{
    if (sceAudioOutInit() != 0) {
        log_printf("sceAudioOutInit a echoue");
        return false;
    }
    if (pthread_create(&g_out_thread, NULL, audio_out_thread, NULL) != 0)
        return false;
    return pthread_create(&g_net_thread, NULL, audio_net_thread, NULL) == 0;
}
