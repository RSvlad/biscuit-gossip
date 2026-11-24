/*
 * util.h - time, logging and RNG (lowest layer; owner: Dev B, issue #2).
 *
 * STUB until #2: this header carries the API agreed in issue #2 so that
 * proto/net/membership can compile against it; src/util.c is a minimal
 * working placeholder that #2 replaces.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

#include "common.h"

/* ---- Time (CLOCK_MONOTONIC only; never use wall-clock for logic) -------- */
uint64_t now_ms(void);

/* ---- Logging (everything goes to stderr, D9) ----------------------------- */
typedef enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG } log_level_t;

void log_init(log_level_t level, uint32_t node_id);
void log_write(log_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_E(...) log_write(LOG_ERROR, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_D(...) log_write(LOG_DEBUG, __VA_ARGS__)

/* ---- RNG (one deterministic generator for the whole program) ------------- */
void rng_seed(uint64_t seed);
uint32_t rng_u32(void);
uint32_t rng_below(uint32_t n); /* uniform in [0, n); rng_below(0) == 0 */
double rng_unit(void);          /* uniform in [0, 1) */

/* ---- Configuration ---------------------------------------------------------- */
void config_defaults(config_t *cfg);

#endif /* UTIL_H */
