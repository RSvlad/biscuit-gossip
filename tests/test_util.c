/*
 * test_util.c - unit tests for util (issue #2): time, logging, RNG, config.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "util.h"

/* ---- RNG: independent processes (must run before the first rng_seed) ------ */

/* 7 processes started at the same moment, none of them calls rng_seed():
 * their sequences must all differ (spec: same seed in all 7 breaks gossip). */
static void test_unseeded_processes_differ(void)
{
    enum { KIDS = 7, N = 4 };
    int fds[KIDS][2];
    pid_t pids[KIDS];
    uint32_t seq[KIDS][N];
    int ok[KIDS];

    for (int k = 0; k < KIDS; k++) {
        ok[k] = 0;
        pids[k] = -1;
        if (pipe(fds[k]) != 0)
            continue;
        pids[k] = fork();
        if (pids[k] == 0) { /* child: only async-signal-safe calls */
            uint32_t out[N];
            for (int i = 0; i < N; i++)
                out[i] = rng_u32();
            ssize_t w = write(fds[k][1], out, sizeof out);
            _exit(w == (ssize_t)sizeof out ? 0 : 1);
        }
        close(fds[k][1]);
    }
    for (int k = 0; k < KIDS; k++) {
        if (pids[k] <= 0)
            continue;
        ok[k] = read(fds[k][0], seq[k], sizeof seq[k]) == (ssize_t)sizeof seq[k];
        close(fds[k][0]);
        int st = 0;
        waitpid(pids[k], &st, 0);
    }
    for (int a = 0; a < KIDS; a++) {
        CHECK(ok[a]);
        for (int b = a + 1; b < KIDS; b++)
            if (ok[a] && ok[b])
                CHECK(memcmp(seq[a], seq[b], sizeof seq[a]) != 0);
    }
}

/* ---- time ------------------------------------------------------------------ */

static void test_now_ms(void)
{
    uint64_t prev = now_ms();
    int monotonic = 1;
    for (int i = 0; i < 200000; i++) {
        uint64_t t = now_ms();
        if (t < prev)
            monotonic = 0;
        prev = t;
    }
    CHECK(monotonic);

    uint64_t a = now_ms();
    struct timespec d = {0, 20 * 1000 * 1000}; /* 20 ms */
    nanosleep(&d, NULL);
    uint64_t b = now_ms();
    CHECK(b >= a + 15 && b < a + 2000);
}

/* ---- RNG -------------------------------------------------------------------- */

static void test_rng_determinism(void)
{
    rng_seed(0); /* splitmix64 reference: first 64-bit output for seed 0 is 0xE220A8397B1DCDAF */
    CHECK_EQ_U64(rng_u32(), 0xE220A839u);

    uint32_t first[16];
    rng_seed(42);
    for (int i = 0; i < 16; i++)
        first[i] = rng_u32();
    rng_seed(42);
    int same = 1;
    for (int i = 0; i < 16; i++)
        if (rng_u32() != first[i])
            same = 0;
    CHECK(same);

    rng_seed(43);
    int differ = 0;
    for (int i = 0; i < 16; i++)
        if (rng_u32() != first[i])
            differ = 1;
    CHECK(differ);
}

static void test_rng_below(void)
{
    rng_seed(42);
    CHECK_EQ_U64(rng_below(0), 0);
    for (int i = 0; i < 100; i++)
        CHECK_EQ_U64(rng_below(1), 0);

    enum { BUCKETS = 7, DRAWS = 700000 };
    unsigned long count[BUCKETS] = {0};
    int out_of_range = 0;
    for (int i = 0; i < DRAWS; i++) {
        uint32_t v = rng_below(BUCKETS);
        if (v >= BUCKETS)
            out_of_range++;
        else
            count[v]++;
    }
    CHECK_EQ_U64(out_of_range, 0);
    for (int b = 0; b < BUCKETS; b++) /* expected 100000 each, sigma ~ 293 */
        CHECK(count[b] > 98000 && count[b] < 102000);

    int big_ok = 1; /* n close to 2^32: the rejection loop must still terminate and stay < n */
    for (int i = 0; i < 1000; i++)
        if (rng_below(0x80000001u) >= 0x80000001u)
            big_ok = 0;
    CHECK(big_ok);
}

static void test_rng_unit(void)
{
    rng_seed(42);
    double sum = 0;
    int in_range = 1;
    enum { DRAWS = 100000 };
    for (int i = 0; i < DRAWS; i++) {
        double x = rng_unit();
        if (!(x >= 0.0 && x < 1.0))
            in_range = 0;
        sum += x;
    }
    CHECK(in_range);
    CHECK(sum / DRAWS > 0.49 && sum / DRAWS < 0.51);
}

/* ---- logging (stderr is redirected into a temp file) ----------------------- */

static int cap_fd = -1;
static int cap_saved = -1;
static char cap_path[32];

static int capture_begin(void)
{
    snprintf(cap_path, sizeof cap_path, "%s", "/tmp/test_util_XXXXXX"); /* mkstemp rewrites it */
    cap_fd = mkstemp(cap_path);
    if (cap_fd < 0)
        return 0;
    cap_saved = dup(STDERR_FILENO);
    if (cap_saved < 0 || dup2(cap_fd, STDERR_FILENO) < 0)
        return 0;
    return 1;
}

/* No CHECK may run between capture_begin() and capture_end(): it prints to stderr. */
static size_t capture_end(char *buf, size_t cap)
{
    dup2(cap_saved, STDERR_FILENO);
    close(cap_saved);
    lseek(cap_fd, 0, SEEK_SET);
    ssize_t n = read(cap_fd, buf, cap - 1);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    close(cap_fd);
    unlink(cap_path);
    return (size_t)n;
}

static void test_log_format_and_levels(void)
{
    char out[2048];
    CHECK(capture_begin());
    log_init(LOG_INFO, 3);
    LOG_I("hello %d", 42);
    LOG_D("hidden-debug"); /* below the threshold: must not appear */
    LOG_W("careful");
    capture_end(out, sizeof out);

    /* "[HH:MM:SS.mmm] [INFO ] [n3] hello 42\n" */
    CHECK(out[0] == '[' && out[3] == ':' && out[6] == ':' && out[9] == '.' && out[13] == ']');
    CHECK(strncmp(out + 14, " [INFO ] [n3] hello 42\n", 23) == 0);
    CHECK(strstr(out, "[WARN ] [n3] careful\n") != NULL);
    CHECK(strstr(out, "hidden-debug") == NULL);
}

static void test_log_verbose_and_error(void)
{
    char out[2048];
    CHECK(capture_begin());
    log_init(LOG_DEBUG, 12);
    LOG_D("dbg %s", "on");
    LOG_E("bad");
    capture_end(out, sizeof out);
    CHECK(strstr(out, "[DEBUG] [n12] dbg on\n") != NULL);
    CHECK(strstr(out, "[ERROR] [n12] bad\n") != NULL);
}

static void test_log_truncates_long_message(void)
{
    char big[3000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    char out[4096];
    CHECK(capture_begin());
    log_init(LOG_INFO, 1);
    LOG_I("%s", big);
    LOG_I("after");
    size_t n = capture_end(out, sizeof out);

    char *nl = strchr(out, '\n');
    CHECK(nl != NULL);
    if (nl != NULL) {
        CHECK((size_t)(nl - out) + 1 <= 512); /* first line fits the line buffer */
        CHECK(nl[-1] == '.' && nl[-2] == '.' && nl[-3] == '.'); /* cut is marked */
        CHECK(strstr(nl + 1, "[INFO ] [n1] after\n") != NULL); /* next line is intact */
    }
    CHECK(n < 1100);
}

/* ---- configuration ----------------------------------------------------------- */

static void test_config_defaults(void)
{
    config_t cfg;
    memset(&cfg, 0xFF, sizeof cfg);
    config_defaults(&cfg);
    CHECK_EQ_U64(cfg.gossip_ms, DEF_GOSSIP_MS);
    CHECK_EQ_U64(cfg.fanout, DEF_FANOUT);
    CHECK_EQ_U64(cfg.suspect_ms, DEF_SUSPECT_MS);
    CHECK_EQ_U64(cfg.fail_ms, DEF_FAIL_MS);
    CHECK_EQ_U64(cfg.cleanup_ms, DEF_CLEANUP_MS);
    CHECK(cfg.loss == 0.0);
    CHECK(!cfg.verbose);
    CHECK(!cfg.has_seed);
}

int main(void)
{
    test_unseeded_processes_differ(); /* first: no rng_seed() has been called yet */
    test_now_ms();
    test_rng_determinism();
    test_rng_below();
    test_rng_unit();
    test_log_format_and_levels();
    test_log_verbose_and_error();
    test_log_truncates_long_message();
    test_config_defaults();
    TEST_MAIN_END();
}
