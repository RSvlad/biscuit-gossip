/*
 * util.c - time, logging and RNG (issue #2).  See util.h for the contract.
 *
 * Only this file (and node.c) may read CLOCK_REALTIME, print directly or
 * abort() (see tests/lint.sh).  No dynamic allocation.
 */
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- time ---------------------------------------------------------------- */

uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        LOG_E("clock_gettime(CLOCK_MONOTONIC) failed: %s", strerror(errno));
        abort(); /* must never happen; logic cannot continue without a clock */
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- logging ------------------------------------------------------------- */

#define LOG_LINE_MAX 512 /* whole line incl. '\n'; well below PIPE_BUF, so one write is atomic */

static log_level_t g_level = LOG_INFO;
static uint32_t g_node_id = 0;

void log_init(log_level_t level, uint32_t node_id)
{
    g_level = level;
    g_node_id = node_id;
}

/* One write() for the whole line; retried only if interrupted by a signal. */
static void write_stderr(const char *buf, size_t len)
{
    ssize_t r;
    do {
        r = write(STDERR_FILENO, buf, len);
    } while (r < 0 && errno == EINTR);
    /* Nothing sensible to do about any other failure of the logger itself. */
}

void log_write(log_level_t level, const char *fmt, ...)
{
    static const char *const names[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
    if (level > LOG_DEBUG || level > g_level)
        return;

    /* Wall clock for display only; never used for logic. */
    int hh = 0, mm = 0, ss = 0;
    long ms = 0;
    struct timespec ts;
    struct tm tmv;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && localtime_r(&ts.tv_sec, &tmv) != NULL) {
        hh = tmv.tm_hour;
        mm = tmv.tm_min;
        ss = tmv.tm_sec;
        ms = ts.tv_nsec / 1000000L;
    }

    char line[LOG_LINE_MAX];
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03ld] [%s] [n%u] ", hh, mm, ss, ms,
                     names[level], (unsigned)g_node_id);
    if (n < 0 || (size_t)n >= sizeof line - 1)
        return;

    /* Body: leave the last byte of the buffer for '\n'. */
    size_t cap = sizeof line - (size_t)n - 1; /* room for body and its NUL */
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, cap, fmt, ap);
    va_end(ap);
    if (m < 0)
        return;

    size_t len = (size_t)n + (size_t)m;
    if ((size_t)m >= cap) { /* truncated: keep the line, mark the cut */
        len = (size_t)n + cap - 1;
        memcpy(line + len - 3, "...", 3);
    }
    line[len++] = '\n';
    write_stderr(line, len);
}

/* ---- RNG: splitmix64 ----------------------------------------------------- */

static uint64_t g_rng_state;
static int g_rng_seeded = 0;

/* splitmix64 output function: a bijection with good avalanche. */
static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void rng_seed(uint64_t seed)
{
    g_rng_state = seed;
    g_rng_seeded = 1;
}

/* Initial seed when rng_seed() was not called: every input is hashed in turn,
 * so processes that differ only in pid or in a few nanoseconds still get
 * unrelated sequences (a plain XOR of small numbers would not guarantee it). */
static uint64_t entropy_seed(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { /* display-grade clock; pid still differs */
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    uint64_t h = mix64((uint64_t)getpid());
    h = mix64(h ^ now_ms());
    h = mix64(h ^ (uint64_t)ts.tv_nsec);
    h = mix64(h ^ (uint64_t)ts.tv_sec);
    return h;
}

static uint64_t rng_next64(void)
{
    if (!g_rng_seeded)
        rng_seed(entropy_seed());
    g_rng_state += 0x9E3779B97F4A7C15ull;
    return mix64(g_rng_state);
}

uint32_t rng_u32(void)
{
    return (uint32_t)(rng_next64() >> 32);
}

uint32_t rng_below(uint32_t n)
{
    if (n == 0)
        return 0;
    uint32_t threshold = (0u - n) % n; /* 2^32 mod n: values below it would bias the result */
    uint32_t r;
    do {
        r = rng_u32();
    } while (r < threshold);
    return r % n;
}

double rng_unit(void)
{
    return (double)(rng_next64() >> 11) * (1.0 / 9007199254740992.0); /* 53 bits / 2^53 */
}

/* ---- configuration ------------------------------------------------------- */

void config_defaults(config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->gossip_ms = DEF_GOSSIP_MS;
    cfg->fanout = DEF_FANOUT;
    cfg->suspect_ms = DEF_SUSPECT_MS;
    cfg->fail_ms = DEF_FAIL_MS;
    cfg->cleanup_ms = DEF_CLEANUP_MS;
}
