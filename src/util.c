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
    log_printf("SysDVR-PS4 " APP_VERSION " demarre");
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
    c->threads = 3;
    c->fast = false;
    c->audio = true;
    c->fullscreen = true;
    c->screen_off = false;
    c->audio_batch = 2;
    c->audio_buffer_ms = 80;
}

static bool parse_bool(const char *v)
{
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || v[0] == 'o' || v[0] == 'O';
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

        // retire espaces et fins de ligne
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) e[-1] = 0;

        if (!strcmp(key, "ip")) {
            strncpy(c->ip, val, sizeof(c->ip) - 1);
            if (!strcmp(c->ip, "auto")) c->ip[0] = 0;
        }
        else if (!strcmp(key, "threads")) c->threads = clampi(atoi(val), 1, 6);
        else if (!strcmp(key, "fast")) c->fast = parse_bool(val);
        else if (!strcmp(key, "audio")) c->audio = parse_bool(val);
        else if (!strcmp(key, "fullscreen")) c->fullscreen = parse_bool(val);
        else if (!strcmp(key, "screen_off")) c->screen_off = parse_bool(val);
        else if (!strcmp(key, "audio_batch")) c->audio_batch = clampi(atoi(val), 0, 3);
        else if (!strcmp(key, "audio_buffer_ms")) c->audio_buffer_ms = clampi(atoi(val), 20, 500);
    }
    fclose(f);
}

void config_save(const Config *c)
{
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        log_printf("Impossible d'ecrire %s", CONFIG_PATH);
        return;
    }
    fprintf(f, "# Configuration SysDVR-PS4\n");
    fprintf(f, "# ip = adresse IP de la Switch, ou auto pour la detection automatique\n");
    fprintf(f, "ip = %s\n", c->ip[0] ? c->ip : "auto");
    fprintf(f, "# threads du decodeur video (1-6). Plus = plus fluide mais un peu plus de latence\n");
    fprintf(f, "threads = %d\n", c->threads);
    fprintf(f, "# fast = 1 desactive le filtre de deblocage (plus rapide)\n");
    fprintf(f, "fast = %d\n", c->fast);
    fprintf(f, "audio = %d\n", c->audio);
    fprintf(f, "fullscreen = %d\n", c->fullscreen);
    fprintf(f, "# eteint l'ecran de la Switch pendant le stream (necessite SysDVR 6.1+)\n");
    fprintf(f, "screen_off = %d\n", c->screen_off);
    fprintf(f, "audio_batch = %d\n", c->audio_batch);
    fprintf(f, "audio_buffer_ms = %d\n", c->audio_buffer_ms);
    fclose(f);
}
