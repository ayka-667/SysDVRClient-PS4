#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define APP_VERSION "1.0"
#define DATA_DIR    "/data/sysdvr-ps4"
#define CONFIG_PATH DATA_DIR "/config.ini"
#define LOG_PATH    DATA_DIR "/log.txt"

typedef struct {
    char ip[64];       // vide = detection automatique
    int  threads;      // threads du decodeur H.264 (1-6)
    bool fast;         // saute le deblocking (plus rapide, un peu moins joli)
    bool audio;        // active le son
    bool fullscreen;   // 720p etire en 1080p, sinon 1:1 centre
    bool screen_off;   // eteint l'ecran de la Switch pendant le stream (SysDVR >= 6.1)
    int  audio_batch;  // 0-3
    int  audio_buffer_ms;
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

    // IP de la Switch actuellement ciblee (manuelle ou detectee)
    char target_ip[64];
    char serial[64];
    // incremente pour forcer les threads reseau a se reconnecter
    volatile int generation;
    volatile bool quit;

    LinkState video_state;
    LinkState audio_state;
    char message[256];

    // statistiques
    volatile uint64_t video_bytes;
    volatile uint32_t video_frames;
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
