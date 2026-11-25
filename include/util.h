/*
 * util.h - time, logging and RNG (lowest layer; owner: Dev B, issue #2).
 *
 * Everything here is independent of node_t, so net/proto/membership/failure
 * may call it freely.  No dynamic allocation anywhere.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

#include "common.h"

/* ---- Time ------------------------------------------------------------------
 * CLOCK_MONOTONIC in milliseconds.  Use it for ALL logic (timers, silence,
 * last_update_ms); wall-clock time is only for display in the log and for the
 * incarnation (#6).  If clock_gettime() fails it logs LOG_ERROR and abort()s:
 * that must never happen.
 */
uint64_t now_ms(void);

/* ---- Logging (everything goes to stderr, D9; never to stdout) -------------
 * Line format:  [HH:MM:SS.mmm] [INFO ] [n3] message
 * The line is formatted into a local buffer and written with a single write(),
 * so lines from different processes do not interleave; messages that do not
 * fit are truncated (marked with "..."), never overflowed.
 * log_init() sets the threshold: messages with a level above it are dropped.
 * main passes LOG_DEBUG only for --verbose, otherwise LOG_INFO.  Before
 * log_init() the defaults are LOG_INFO and node id 0.
 */
typedef enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG } log_level_t;

void log_init(log_level_t level, uint32_t node_id);
void log_write(log_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_E(...) log_write(LOG_ERROR, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_D(...) log_write(LOG_DEBUG, __VA_ARGS__)

/* ---- RNG (one small deterministic generator, splitmix64, for the program) --
 * If rng_seed() is never called, the first use seeds from getpid(), now_ms()
 * and the CLOCK_REALTIME time, so processes started in the same second get
 * different sequences.  Tests call rng_seed(42) for reproducibility.
 */
void rng_seed(uint64_t seed);
uint32_t rng_u32(void);
uint32_t rng_below(uint32_t n); /* uniform in [0, n), no modulo bias; rng_below(0) == 0 */
double rng_unit(void);          /* uniform in [0, 1), e.g. for --loss */

/* ---- Configuration ---------------------------------------------------------- */

/* Zeroes *cfg and fills the DEF_* defaults from common.h (loss 0, verbose off,
 * no seed).  Argument parsing is main's job (#12). */
void config_defaults(config_t *cfg);

#endif /* UTIL_H */
