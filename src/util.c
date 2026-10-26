#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#include <orbis/libkernel.h>

AppShared g_app;

static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

void sleep_ms(int ms)
{
    sceKernelUsleep((uint32_t)ms * 1000);
}

void log_init(void)
{
    mkdir(DATA_DIR, 0777);
    g_log = fopen(LOG_PATH, "w");
    log_printf("SysDVR-PS4 " APP_VERSION " started");
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_log_lock);

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void app_set_message(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_app.lock);
    strncpy(g_app.message, buf, sizeof(g_app.message) - 1);
    g_app.message[sizeof(g_app.message) - 1] = 0;
    pthread_mutex_unlock(&g_app.lock);

    log_printf("%s", buf);
}

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->threads = 2;
    c->fast = false;
    c->audio = true;
    c->fullscreen = true;
    c->screen_off = false;
    c->smooth = true;
    c->audio_batch = 0;
    c->audio_buffer_ms = 60;
    c->max_delay_ms = 150;
}

static bool parse_bool(const char *v)
{
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void config_load(Config *c)
{
    config_defaults(c);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_save(c);
        return;
    }

    int version = 1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p)
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = p;
        char *val = eq + 1;

        // trim spaces and line endings
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) e[-1] = 0;

        if (!strcmp(key, "ip")) {
            strncpy(c->ip, val, sizeof(c->ip) - 1);
            if (!strcmp(c->ip, "auto")) c->ip[0] = 0;
        }
        else if (!strcmp(key, "config_version")) version = atoi(val);
        else if (!strcmp(key, "threads")) c->threads = clampi(atoi(val), 1, 6);
        else if (!strcmp(key, "fast")) c->fast = parse_bool(val);
        else if (!strcmp(key, "audio")) c->audio = parse_bool(val);
        else if (!strcmp(key, "fullscreen")) c->fullscreen = parse_bool(val);
        else if (!strcmp(key, "smooth")) c->smooth = parse_bool(val);
        else if (!strcmp(key, "screen_off")) c->screen_off = parse_bool(val);
        else if (!strcmp(key, "audio_batch")) c->audio_batch = clampi(atoi(val), 0, 3);
        else if (!strcmp(key, "audio_buffer_ms")) c->audio_buffer_ms = clampi(atoi(val), 20, 500);
        else if (!strcmp(key, "max_delay_ms")) c->max_delay_ms = clampi(atoi(val), 50, 1000);
    }
    fclose(f);

    if (version < CONFIG_VERSION) {
        // the 1.0 defaults added latency: move them to the new low-latency values
        Config d;
        config_defaults(&d);
        c->threads = d.threads;
        c->audio_batch = d.audio_batch;
        c->audio_buffer_ms = d.audio_buffer_ms;
        log_printf("Config upgraded from version %d", version);
        config_save(c);
    }
}

void config_save(const Config *c)
{
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        log_printf("Cannot write %s", CONFIG_PATH);
        return;
    }
    fprintf(f, "# SysDVR-PS4 configuration\n");
    fprintf(f, "config_version = %d\n", CONFIG_VERSION);
    fprintf(f, "# ip = Switch IP address, or auto for automatic discovery\n");
    fprintf(f, "ip = %s\n", c->ip[0] ? c->ip : "auto");
    fprintf(f, "# video decoder threads (1-6). Each extra thread decodes faster but adds ~33 ms of latency\n");
    fprintf(f, "threads = %d\n", c->threads);
    fprintf(f, "# fast = 1 disables the deblocking filter (faster decoding, slightly blockier)\n");
    fprintf(f, "fast = %d\n", c->fast);
    fprintf(f, "audio = %d\n", c->audio);
    fprintf(f, "fullscreen = %d\n", c->fullscreen);
    fprintf(f, "# smooth = 0 uses a cheaper scaler\n");
    fprintf(f, "smooth = %d\n", c->smooth);
    fprintf(f, "# turn off the Switch screen while streaming (needs SysDVR 6.1+)\n");
    fprintf(f, "screen_off = %d\n", c->screen_off);
    fprintf(f, "# audio packets batched by the Switch (0-3). 0 = lowest latency\n");
    fprintf(f, "audio_batch = %d\n", c->audio_batch);
    fprintf(f, "# target audio buffer. Raise it if the sound crackles\n");
    fprintf(f, "audio_buffer_ms = %d\n", c->audio_buffer_ms);
    fprintf(f, "# when the video falls this far behind, frames are skipped to catch up\n");
    fprintf(f, "max_delay_ms = %d\n", c->max_delay_ms);
    fclose(f);
}
