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

static pthread_t g_thread;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static AVFrame *g_shared_frame; // derniere image decodee, protegee par g_frame_lock
static bool g_has_new_frame;
static volatile int g_sock = -1;

static AVFrame *g_render_frame;
static struct SwsContext *g_sws;
static int g_sws_src_w, g_sws_src_h, g_sws_src_fmt, g_sws_dst_w, g_sws_dst_h, g_sws_flags;

void video_kick(void)
{
    int fd = g_sock;
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

static AVCodecContext *open_decoder(const Config *cfg)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        log_printf("Decodeur H.264 introuvable");
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;

    // SPS/PPS de la Switch fournis d'avance comme le client officiel
    ctx->extradata_size = sizeof(SDVR_SPS) + sizeof(SDVR_PPS);
    ctx->extradata = av_mallocz(ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, SDVR_SPS, sizeof(SDVR_SPS));
    memcpy(ctx->extradata + sizeof(SDVR_SPS), SDVR_PPS, sizeof(SDVR_PPS));

    ctx->thread_count = cfg->threads;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (cfg->threads <= 1)
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    if (cfg->fast)
        ctx->skip_loop_filter = AVDISCARD_ALL;

    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) {
        log_printf("avcodec_open2 a echoue : %d", rc);
        avcodec_free_context(&ctx);
        return NULL;
    }
    log_printf("Decodeur H.264 ouvert (%d threads, fast=%d)", cfg->threads, cfg->fast);
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

static void decode_packet(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
{
    int rc = avcodec_send_packet(ctx, pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN))
        return;

    while (avcodec_receive_frame(ctx, frame) == 0)
        publish_frame(frame);
}

static void set_state(LinkState s)
{
    pthread_mutex_lock(&g_app.lock);
    g_app.video_state = s;
    pthread_mutex_unlock(&g_app.lock);
}

static void *video_thread(void *arg)
{
    (void)arg;
    uint8_t *buf = av_malloc(SDVR_MAX_PAYLOAD + AV_INPUT_BUFFER_PADDING_SIZE);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVCodecContext *ctx = NULL;
    char err[256];

    while (!g_app.quit) {
        char ip[64];
        Config cfg;
        pthread_mutex_lock(&g_app.lock);
        strcpy(ip, g_app.target_ip);
        cfg = g_app.cfg;
        int gen = g_app.generation;
        pthread_mutex_unlock(&g_app.lock);

        if (!ip[0]) {
            set_state(LINK_IDLE);
            sleep_ms(200);
            continue;
        }

        set_state(LINK_CONNECTING);
        int fd = sdvr_connect(ip, SDVR_PORT_VIDEO, 3000, err, sizeof(err));
        if (fd >= 0 && sdvr_handshake(fd, &(SdvrHandshakeOptions){ .video = true, .audio_batching = cfg.audio_batch, .turn_off_screen = cfg.screen_off }, err, sizeof(err)) != 0) {
            sdvr_close(fd);
            fd = -1;
        }
        if (fd < 0) {
            app_set_message("Video : %s", err);
            set_state(LINK_ERROR);
            for (int i = 0; i < 20 && !g_app.quit && g_app.generation == gen; i++)
                sleep_ms(100);
            continue;
        }

        g_sock = fd;
        log_printf("Video connectee a %s", ip);
        app_set_message("Connecte a la Switch (%s)", ip);
        set_state(LINK_STREAMING);

        // on recree le decodeur a chaque connexion pour repartir d'un etat propre
        if (ctx)
            avcodec_free_context(&ctx);
        ctx = open_decoder(&cfg);
        if (!ctx) {
            app_set_message("Impossible d'initialiser le decodeur video");
            set_state(LINK_ERROR);
            sdvr_close(fd);
            g_sock = -1;
            sleep_ms(2000);
            continue;
        }

        volatile bool stop = false;
        SdvrPacketHeader hdr;
        while (!g_app.quit && g_app.generation == gen) {
            int rc = sdvr_read_packet(fd, &hdr, buf, &stop);
            if (rc < 0) {
                app_set_message("Video : connexion perdue, reconnexion...");
                break;
            }
            if (rc == 0)
                continue; // pas de donnees (aucun jeu lance ?)

            g_app.last_packet_us = now_us();
            g_app.video_bytes += hdr.DataSize + sizeof(hdr);

            if (hdr.MetaData & SDVR_META_ERROR) {
                char msg[200];
                sdvr_format_error(buf, hdr.DataSize, msg, sizeof(msg));
                app_set_message("Switch : %s", msg);
                continue;
            }
            if (!(hdr.MetaData & SDVR_META_VIDEO) || (hdr.MetaData & SDVR_META_REPLAY) || hdr.DataSize == 0)
                continue;

            memset(buf + hdr.DataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            pkt->data = buf;
            pkt->size = (int)hdr.DataSize;
            pkt->pts = (int64_t)hdr.Timestamp;
            pkt->dts = pkt->pts;
            decode_packet(ctx, pkt, frame);
        }

        g_sock = -1;
        sdvr_close(fd);
        set_state(LINK_CONNECTING);
    }

    if (ctx)
        avcodec_free_context(&ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_free(buf);
    return NULL;
}

bool video_start(void)
{
    g_shared_frame = av_frame_alloc();
    g_render_frame = av_frame_alloc();
    if (!g_shared_frame || !g_render_frame)
        return false;
    return pthread_create(&g_thread, NULL, video_thread, NULL) == 0;
}

static void noop_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

bool video_render(uint32_t *fb, bool fullscreen, bool fast)
{
    pthread_mutex_lock(&g_frame_lock);
    bool has = g_has_new_frame;
    if (has) {
        av_frame_unref(g_render_frame);
        av_frame_move_ref(g_render_frame, g_shared_frame);
        g_has_new_frame = false;
    }
    pthread_mutex_unlock(&g_frame_lock);

    if (!has)
        return false;

    AVFrame *src = g_render_frame;
    int dst_w, dst_h;
    if (fullscreen) {
        // garde le ratio de l'image
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

    int flags = fast ? SWS_FAST_BILINEAR : SWS_BILINEAR;
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
        av_opt_set_int(g_sws, "threads", 3, 0);
        if (sws_init_context(g_sws, NULL, NULL) < 0) {
            log_printf("sws_init_context a echoue");
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
        log_printf("Conversion %dx%d -> %dx%d", src->width, src->height, dst_w, dst_h);
    }

    // bandes noires autour de l'image
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

    // l'image de sortie pointe directement dans le framebuffer
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
        log_printf("sws_scale_frame a echoue : %d", rc);
        return false;
    }
    return true;
}
