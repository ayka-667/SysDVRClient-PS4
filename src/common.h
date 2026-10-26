#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define APP_VERSION "1.1"
#define DATA_DIR    "/data/sysdvr-ps4"
#define CONFIG_PATH DATA_DIR "/config.ini"
#define LOG_PATH    DATA_DIR "/log.txt"

// Bumped when the defaults change: older config files get the latency-related keys reset
#define CONFIG_VERSION 2

typedef struct {
    char ip[64];       // empty = automatic discovery
    int  threads;      // H.264 decoder threads (1-6)
    bool fast;         // skip the deblocking filter (faster, slightly blockier)
    bool audio;        // enable sound
    bool fullscreen;   // stretch 720p to 1080p, otherwise 1:1 centred
    bool screen_off;   // turn off the Switch screen while streaming (SysDVR >= 6.1)
    bool smooth;       // bilinear scaling (otherwise the faster fast-bilinear)
    int  audio_batch;  // 0-3
    int  audio_buffer_ms;
    int  max_delay_ms; // video backlog above which frames are dropped to catch up
} Config;

typedef enum {
    LINK_IDLE,
    LINK_CONNECTING,
    LINK_STREAMING,
    LINK_ERROR,
} LinkState;

typedef struct {
    pthread_mutex_t lock;

    Config cfg;

    // Switch IP currently targeted (manual or discovered)
    char target_ip[64];
    char serial[64];
    // incremented to force the network threads to reconnect
    volatile int generation;
    volatile bool quit;

    LinkState video_state;
    LinkState audio_state;
    char message[256];

    // statistics
    volatile uint64_t video_bytes;
    volatile uint32_t video_frames;
    volatile uint32_t video_dropped;   // packets skipped to catch up
    volatile uint32_t video_queue;     // packets waiting to be decoded
    volatile uint32_t decode_us;       // average decode time per frame
    volatile int32_t  video_delay_ms;  // extra delay of the last shown frame over the best seen
    volatile uint32_t audio_fill_ms;
    volatile uint32_t audio_underruns;
    volatile uint64_t last_packet_us;
    volatile uint64_t last_frame_us;
} AppShared;

extern AppShared g_app;

void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t now_us(void);
void sleep_ms(int ms);

void config_defaults(Config *c);
void config_load(Config *c);
void config_save(const Config *c);

void app_set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
