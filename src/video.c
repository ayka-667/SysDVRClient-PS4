// Video pipeline: network thread -> packet queue -> decoder thread -> latest frame -> main thread (scale + display).
// The network thread always drains the socket so a backlog never builds up inside TCP: when the queue gets too old
// it is thrown away and decoding resumes at the next keyframe, which keeps the latency bounded.
#include "video.h"
#include "common.h"
#include "gfx.h"
#include "sysdvr_proto.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define QUEUE_CAP          64
#define KEYFRAME_WAIT_US   2000000 // give up waiting for a keyframe after this long

#ifdef SYSDVR_TEST
extern int g_test_decode_delay_ms;
#endif

typedef struct {
    AVPacket *pkt;
    uint64_t recv_us;
    int session;
    bool flush; // decoder must be flushed before this packet (frames were dropped)
} QueueItem;

static pthread_t g_net_thread, g_dec_thread;
static volatile int g_sock = -1;

// packet queue, protected by g_queue_lock
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static QueueItem g_queue[QUEUE_CAP];
static int g_queue_head, g_queue_count;
static int g_session;
static bool g_wait_key;
static uint64_t g_wait_key_since;

static uint64_t g_last_key_us;
static int g_keys_logged;

// best (smallest) local arrival time minus Switch timestamp seen this session
static volatile int64_t g_clock_offset;

// latest decoded frame, protected by g_frame_lock
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static AVFrame *g_shared_frame;
static bool g_has_new_frame;

// main thread only
static AVFrame *g_render_frame;
static bool g_render_valid;
static struct SwsContext *g_sws;
static int g_sws_src_w, g_sws_src_h, g_sws_src_fmt, g_sws_dst_w, g_sws_dst_h, g_sws_flags;

void video_kick(void)
{
    int fd = g_sock;
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

// ---------------------------------------------------------------- queue

static void queue_clear_locked(void)
{
    while (g_queue_count > 0) {
        av_packet_free(&g_queue[g_queue_head].pkt);
        g_queue_head = (g_queue_head + 1) % QUEUE_CAP;
        g_queue_count--;
    }
    g_app.video_queue = 0;
}

static void queue_new_session(void)
{
    pthread_mutex_lock(&g_queue_lock);
    queue_clear_locked();
    g_session++;
    g_wait_key = false;
    g_clock_offset = INT64_MAX;
    g_last_key_us = 0;
    g_keys_logged = 0;
    pthread_mutex_unlock(&g_queue_lock);
}

// Takes ownership of pkt
static void queue_push(AVPacket *pkt, bool key, int max_delay_ms)
{
    uint64_t t = now_us();
    bool flush = false;

    pthread_mutex_lock(&g_queue_lock);

    // too far behind: drop everything queued and restart at the next keyframe
    if (g_queue_count > 0) {
        uint64_t age = t - g_queue[g_queue_head].recv_us;
        if (g_queue_count >= QUEUE_CAP || age > (uint64_t)max_delay_ms * 1000) {
            g_app.video_dropped += g_queue_count;
            log_printf("Video %d ms behind (%d packets queued): skipping to the next keyframe", (int)(age / 1000), g_queue_count);
            queue_clear_locked();
            if (!g_wait_key) {
                g_wait_key = true;
                g_wait_key_since = t;
            }
        }
    }

    if (g_wait_key) {
        if (!key && t - g_wait_key_since < KEYFRAME_WAIT_US) {
            g_app.video_dropped++;
            pthread_mutex_unlock(&g_queue_lock);
            av_packet_free(&pkt);
            return;
        }
        // resume on a keyframe with a clean decoder; after a timeout keep the old references instead
        flush = key;
        g_wait_key = false;
    }

    QueueItem *it = &g_queue[(g_queue_head + g_queue_count) % QUEUE_CAP];
    it->pkt = pkt;
    it->recv_us = t;
    it->session = g_session;
    it->flush = flush;
    g_queue_count++;
    g_app.video_queue = g_queue_count;
    pthread_mutex_unlock(&g_queue_lock);
}

static bool queue_pop(QueueItem *out, int *remaining)
{
    bool ok = false;
    pthread_mutex_lock(&g_queue_lock);
    if (g_queue_count > 0) {
        *out = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % QUEUE_CAP;
        g_queue_count--;
        *remaining = g_queue_count;
        g_app.video_queue = g_queue_count;
        ok = true;
    }
    pthread_mutex_unlock(&g_queue_lock);
    return ok;
}

// IDR slice (NAL type 5) before any non-IDR slice
static bool is_keyframe(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int type = d[i + 3] & 0x1F;
            if (type == 5)
                return true;
            if (type == 1)
                return false;
            i += 2;
        }
    }
    return false;
}

// ---------------------------------------------------------------- network thread (TCP) and USB entry points

static void set_state(LinkState s)
{
    pthread_mutex_lock(&g_app.lock);
    g_app.video_state = s;
    pthread_mutex_unlock(&g_app.lock);
}

// Everything after a packet has been received, shared by TCP and USB. data is the payload; pkt (video only) owns a copy of it.
static void handle_packet(const SdvrPacketHeader *hdr, const uint8_t *data, AVPacket *pkt)
{
    uint64_t t = now_us();
    g_app.last_packet_us = t;
    g_app.video_bytes += hdr->DataSize + sizeof(*hdr);

    if (hdr->MetaData & SDVR_META_ERROR) {
        char msg[200];
        sdvr_format_error(data, hdr->DataSize, msg, sizeof(msg));
        app_set_message("Switch: %s", msg);
        av_packet_free(&pkt);
        return;
    }
    if (!pkt)
        return;

    int64_t off = (int64_t)t - (int64_t)hdr->Timestamp;
    if (off < g_clock_offset)
        g_clock_offset = off;

    pkt->pts = pkt->dts = (int64_t)hdr->Timestamp;
    bool key = is_keyframe(pkt->data, (size_t)pkt->size);
    if (key) {
        if (g_last_key_us && g_keys_logged < 5) {
            log_printf("Keyframe interval: %d ms", (int)((t - g_last_key_us) / 1000));
            g_keys_logged++;
        }
        g_last_key_us = t;
    }
    queue_push(pkt, key, g_app.cfg.max_delay_ms);
}

static bool is_video_packet(const SdvrPacketHeader *hdr)
{
    return (hdr->MetaData & SDVR_META_VIDEO) && !(hdr->MetaData & (SDVR_META_REPLAY | SDVR_META_ERROR)) && hdr->DataSize > 0;
}

void video_usb_begin(void)
{
    queue_new_session();
}

void video_usb_packet(const SdvrPacketHeader *hdr, const uint8_t *data)
{
    AVPacket *pkt = NULL;
    if (is_video_packet(hdr)) {
        pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, (int)hdr->DataSize) < 0)
            av_packet_free(&pkt);
        else
            memcpy(pkt->data, data, hdr->DataSize);
    }
    handle_packet(hdr, data, pkt);
}

static void *video_net_thread(void *arg)
{
    (void)arg;
    uint8_t *scratch = malloc(SDVR_MAX_PAYLOAD);
    char err[256];

    while (!g_app.quit) {
        char ip[64];
        Config cfg;
        pthread_mutex_lock(&g_app.lock);
        strcpy(ip, g_app.target_ip);
        cfg = g_app.cfg;
        int gen = g_app.generation;
        pthread_mutex_unlock(&g_app.lock);

        if (cfg.usb) {
            // the USB thread does the streaming
            sleep_ms(200);
            continue;
        }
        if (!ip[0]) {
            set_state(LINK_IDLE);
            sleep_ms(200);
            continue;
        }

        set_state(LINK_CONNECTING);
        int fd = sdvr_connect(ip, SDVR_PORT_VIDEO, 3000, err, sizeof(err));
        if (fd >= 0 && sdvr_handshake(fd, &(SdvrHandshakeOptions){ .video = true, .audio = false, .audio_batching = cfg.audio_batch, .turn_off_screen = cfg.screen_off }, err, sizeof(err)) != 0) {
            sdvr_close(fd);
            fd = -1;
        }
        if (fd < 0) {
            app_set_message("Video: %s", err);
            set_state(LINK_ERROR);
            for (int i = 0; i < 20 && !g_app.quit && g_app.generation == gen; i++)
                sleep_ms(100);
            continue;
        }

        g_sock = fd;
        queue_new_session();
        log_printf("Video connected to %s", ip);
        app_set_message("Connected to the Switch (%s)", ip);
        set_state(LINK_STREAMING);

        volatile bool stop = false;
        SdvrPacketHeader hdr;
        while (!g_app.quit && g_app.generation == gen && !g_app.cfg.usb) {
            int rc = sdvr_read_header(fd, &hdr, &stop);
            if (rc < 0) {
                app_set_message("Video: connection lost, reconnecting...");
                break;
            }
            if (rc == 0)
                continue; // no data (no game running?)

            bool is_video = is_video_packet(&hdr);
            AVPacket *pkt = NULL;
            uint8_t *dst = scratch;
            if (is_video) {
                pkt = av_packet_alloc();
                if (!pkt || av_new_packet(pkt, (int)hdr.DataSize) < 0) {
                    av_packet_free(&pkt);
                    is_video = false;
                } else {
                    dst = pkt->data;
                }
            }
            if (sdvr_read_payload(fd, dst, hdr.DataSize, &stop) != 1) {
                av_packet_free(&pkt);
                app_set_message("Video: connection lost, reconnecting...");
                break;
            }

            handle_packet(&hdr, dst, pkt);
        }

        g_sock = -1;
        sdvr_close(fd);
        set_state(LINK_CONNECTING);
    }

    free(scratch);
    return NULL;
}

// ---------------------------------------------------------------- decoder thread

static AVCodecContext *open_decoder(const Config *cfg)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        log_printf("H.264 decoder not found");
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;

    // the Switch's SPS/PPS given up front, like the official client
    ctx->extradata_size = sizeof(SDVR_SPS) + sizeof(SDVR_PPS);
    ctx->extradata = av_mallocz(ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, SDVR_SPS, sizeof(SDVR_SPS));
    memcpy(ctx->extradata + sizeof(SDVR_SPS), SDVR_PPS, sizeof(SDVR_PPS));

    // frame threads add one frame of delay each; LOW_DELAY would disable them, so only use it with 1 thread
    ctx->thread_count = cfg->threads;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (cfg->threads <= 1)
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    if (cfg->fast)
        ctx->skip_loop_filter = AVDISCARD_ALL;

    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) {
        log_printf("avcodec_open2 failed: %d", rc);
        avcodec_free_context(&ctx);
        return NULL;
    }
    log_printf("H.264 decoder opened (%d threads, fast=%d)", cfg->threads, cfg->fast);
    return ctx;
}

static void publish_frame(AVFrame *frame)
{
    pthread_mutex_lock(&g_frame_lock);
    av_frame_unref(g_shared_frame);
    av_frame_move_ref(g_shared_frame, frame);
    g_has_new_frame = true;
    pthread_mutex_unlock(&g_frame_lock);
    g_app.video_frames++;
    g_app.last_frame_us = now_us();
}

static void *video_dec_thread(void *arg)
{
    (void)arg;
    AVFrame *frame = av_frame_alloc();
    AVCodecContext *ctx = NULL;
    int session = -1;
    bool fast = false;
    uint32_t avg_us = 0;

    while (!g_app.quit) {
        QueueItem it;
        int remaining;
        if (!queue_pop(&it, &remaining)) {
            sleep_ms(1);
            continue;
        }

        if (it.session != session) {
            // fresh decoder for every connection, with the settings of that connection
            pthread_mutex_lock(&g_app.lock);
            Config cfg = g_app.cfg;
            pthread_mutex_unlock(&g_app.lock);
            if (ctx)
                avcodec_free_context(&ctx);
            ctx = open_decoder(&cfg);
            if (!ctx)
                app_set_message("Could not initialise the video decoder");
            session = it.session;
            fast = cfg.fast;
            avg_us = 0;
        }
        if (!ctx) {
            av_packet_free(&it.pkt);
            continue;
        }

        if (it.flush)
            avcodec_flush_buffers(ctx);
        // falling behind: skip the deblocking filter until the queue is drained
        ctx->skip_loop_filter = (fast || remaining >= 2) ? AVDISCARD_ALL : AVDISCARD_DEFAULT;

        uint64_t t0 = now_us();
        int rc = avcodec_send_packet(ctx, it.pkt);
        av_packet_free(&it.pkt);
        if (rc >= 0 || rc == AVERROR(EAGAIN)) {
            while (avcodec_receive_frame(ctx, frame) == 0)
                publish_frame(frame);
        }
#ifdef SYSDVR_TEST
        if (g_test_decode_delay_ms)
            sleep_ms(g_test_decode_delay_ms);
#endif
        uint32_t dt = (uint32_t)(now_us() - t0);
        avg_us = avg_us ? (avg_us * 15 + dt) / 16 : dt;
        g_app.decode_us = avg_us;
    }

    if (ctx)
        avcodec_free_context(&ctx);
    av_frame_free(&frame);
    return NULL;
}

bool video_start(void)
{
    g_shared_frame = av_frame_alloc();
    g_render_frame = av_frame_alloc();
    if (!g_shared_frame || !g_render_frame)
        return false;
    g_clock_offset = INT64_MAX;
    if (pthread_create(&g_dec_thread, NULL, video_dec_thread, NULL) != 0)
        return false;
    return pthread_create(&g_net_thread, NULL, video_net_thread, NULL) == 0;
}

// ---------------------------------------------------------------- rendering (main thread)

bool video_has_new_frame(void)
{
    pthread_mutex_lock(&g_frame_lock);
    bool has = g_has_new_frame;
    pthread_mutex_unlock(&g_frame_lock);
    return has;
}

static void noop_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

bool video_render(uint32_t *fb, bool fullscreen, int scaler, bool force)
{
    pthread_mutex_lock(&g_frame_lock);
    bool has = g_has_new_frame;
    if (has) {
        av_frame_unref(g_render_frame);
        av_frame_move_ref(g_render_frame, g_shared_frame);
        g_has_new_frame = false;
        g_render_valid = true;
    }
    pthread_mutex_unlock(&g_frame_lock);

    if (!has && !(force && g_render_valid))
        return false;

    AVFrame *src = g_render_frame;
    if (has && g_clock_offset != INT64_MAX && src->pts != AV_NOPTS_VALUE)
        g_app.video_delay_ms = (int32_t)(((int64_t)now_us() - src->pts - g_clock_offset) / 1000);

    int dst_w, dst_h;
    if (fullscreen) {
        // keep the aspect ratio
        dst_w = SCREEN_W;
        dst_h = (int)((int64_t)SCREEN_W * src->height / src->width);
        if (dst_h > SCREEN_H) {
            dst_h = SCREEN_H;
            dst_w = (int)((int64_t)SCREEN_H * src->width / src->height);
        }
    } else {
        dst_w = src->width < SCREEN_W ? src->width : SCREEN_W;
        dst_h = src->height < SCREEN_H ? src->height : SCREEN_H;
    }
    dst_w &= ~1;
    dst_h &= ~1;

    int flags = scaler == SCALER_SHARP ? SWS_BICUBIC : scaler == SCALER_SMOOTH ? SWS_BILINEAR : SWS_FAST_BILINEAR;
    if (!g_sws || g_sws_src_w != src->width || g_sws_src_h != src->height || g_sws_src_fmt != src->format ||
        g_sws_dst_w != dst_w || g_sws_dst_h != dst_h || g_sws_flags != flags) {
        if (g_sws)
            sws_freeContext(g_sws);
        g_sws = sws_alloc_context();
        av_opt_set_int(g_sws, "srcw", src->width, 0);
        av_opt_set_int(g_sws, "srch", src->height, 0);
        av_opt_set_int(g_sws, "src_format", src->format, 0);
        av_opt_set_int(g_sws, "dstw", dst_w, 0);
        av_opt_set_int(g_sws, "dsth", dst_h, 0);
        av_opt_set_int(g_sws, "dst_format", AV_PIX_FMT_BGRA, 0);
        av_opt_set_int(g_sws, "sws_flags", flags, 0);
        if (scaler == SCALER_SHARP) {
            // B = 0, C = 0.75: a bicubic with a bit of sharpening, ~35% more expensive than bilinear
            av_opt_set_double(g_sws, "param0", 0.0, 0);
            av_opt_set_double(g_sws, "param1", 0.75, 0);
        }
        av_opt_set_int(g_sws, "threads", 3, 0);
        if (sws_init_context(g_sws, NULL, NULL) < 0) {
            log_printf("sws_init_context failed");
            sws_freeContext(g_sws);
            g_sws = NULL;
            return false;
        }
        g_sws_src_w = src->width;
        g_sws_src_h = src->height;
        g_sws_src_fmt = src->format;
        g_sws_dst_w = dst_w;
        g_sws_dst_h = dst_h;
        g_sws_flags = flags;
        log_printf("Scaling %dx%d -> %dx%d", src->width, src->height, dst_w, dst_h);
    }

    // black bars around the picture
    int ox = (SCREEN_W - dst_w) / 2;
    int oy = (SCREEN_H - dst_h) / 2;
    if (oy > 0) {
        gfx_rect(fb, 0, 0, SCREEN_W, oy, RGB(0, 0, 0));
        gfx_rect(fb, 0, oy + dst_h, SCREEN_W, SCREEN_H - oy - dst_h, RGB(0, 0, 0));
    }
    if (ox > 0) {
        gfx_rect(fb, 0, oy, ox, dst_h, RGB(0, 0, 0));
        gfx_rect(fb, ox + dst_w, oy, SCREEN_W - ox - dst_w, dst_h, RGB(0, 0, 0));
    }

    // the output frame points straight into the framebuffer
    AVFrame *dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_BGRA;
    dst->width = dst_w;
    dst->height = dst_h;
    dst->data[0] = (uint8_t *)(fb + (size_t)oy * SCREEN_W + ox);
    dst->linesize[0] = SCREEN_W * 4;
    dst->buf[0] = av_buffer_create(dst->data[0], (size_t)SCREEN_W * 4 * dst_h, noop_free, NULL, 0);

    int rc = sws_scale_frame(g_sws, dst, src);
    av_frame_free(&dst);
    if (rc < 0) {
        log_printf("sws_scale_frame failed: %d", rc);
        return false;
    }
    return true;
}
