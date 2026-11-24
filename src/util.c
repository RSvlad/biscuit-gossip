/* STUB until #2: minimal working util; Dev B replaces/extends it in #2. */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fputs("fatal: clock_gettime(CLOCK_MONOTONIC) failed\n", stderr);
        abort();
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- logging ---- */

static log_level_t g_level = LOG_INFO;
static uint32_t g_node_id = 0;

void log_init(log_level_t level, uint32_t node_id)
{
    g_level = level;
    g_node_id = node_id;
}

void log_write(log_level_t level, const char *fmt, ...)
{
    static const char *const names[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
    if (level > g_level)
        return;

    char line[512];
    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts); /* display only */
    localtime_r(&ts.tv_sec, &tmv);

    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03ld] [%s] [n%u] ", tmv.tm_hour,
                     tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L, names[level],
                     (unsigned)g_node_id);
    if (n < 0 || (size_t)n >= sizeof line)
        return;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        return;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof line - 2)
        len = sizeof line - 2; /* truncated */
    line[len++] = '\n';
    fwrite(line, 1, len, stderr);
}

/* ---- RNG: splitmix64 ---- */

static uint64_t g_rng_state;
static int g_rng_seeded = 0;

void rng_seed(uint64_t seed)
{
    g_rng_state = seed;
    g_rng_seeded = 1;
}

static uint64_t rng_next64(void)
{
    if (!g_rng_seeded) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        rng_seed(((uint64_t)getpid() << 32) ^ now_ms() ^ (uint64_t)ts.tv_nsec);
    }
    uint64_t z = (g_rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint32_t rng_u32(void)
{
    return (uint32_t)(rng_next64() >> 32);
}

uint32_t rng_below(uint32_t n)
{
    if (n == 0)
        return 0;
    uint32_t threshold = (0u - n) % n; /* rejection sampling: no modulo bias */
    uint32_t r;
    do {
        r = rng_u32();
    } while (r < threshold);
    return r % n;
}

double rng_unit(void)
{
    return (double)(rng_next64() >> 11) * (1.0 / 9007199254740992.0);
}

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->gossip_ms = DEF_GOSSIP_MS;
    cfg->fanout = DEF_FANOUT;
    cfg->suspect_ms = DEF_SUSPECT_MS;
    cfg->fail_ms = DEF_FAIL_MS;
    cfg->cleanup_ms = DEF_CLEANUP_MS;
}
