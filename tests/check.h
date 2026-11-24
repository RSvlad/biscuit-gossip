/*
 * check.h - minimal unit-test macros (no external framework).
 *
 *   #include "check.h"
 *   int main(void) {
 *       CHECK(1 + 1 == 2);
 *       CHECK_EQ_U64(mem_count(&n), 3);
 *       CHECK_STR_EQ(state_name(STATE_ALIVE), "ALIVE");
 *       TEST_MAIN_END();
 *   }
 *
 * A failed check prints file:line and the expression, and the run continues;
 * TEST_MAIN_END() returns non-zero from main() if any check failed.
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int check_total_ = 0;
static int check_failed_ = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        check_total_++;                                                          \
        if (!(cond)) {                                                           \
            check_failed_++;                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,     \
                    #cond);                                                      \
        }                                                                        \
    } while (0)

/* Both sides are converted to uint64_t and evaluated exactly once. */
#define CHECK_EQ_U64(a, b)                                                       \
    do {                                                                         \
        uint64_t check_a_ = (uint64_t)(a);                                       \
        uint64_t check_b_ = (uint64_t)(b);                                       \
        check_total_++;                                                          \
        if (check_a_ != check_b_) {                                              \
            check_failed_++;                                                     \
            fprintf(stderr, "%s:%d: CHECK_EQ_U64 failed: %s == %s (%llu vs %llu)\n", \
                    __FILE__, __LINE__, #a, #b, (unsigned long long)check_a_,    \
                    (unsigned long long)check_b_);                               \
        }                                                                        \
    } while (0)

/* NULL is equal only to NULL. */
#define CHECK_STR_EQ(a, b)                                                       \
    do {                                                                         \
        const char *check_sa_ = (a);                                             \
        const char *check_sb_ = (b);                                             \
        check_total_++;                                                          \
        if (check_sa_ == NULL || check_sb_ == NULL ? check_sa_ != check_sb_      \
                                                   : strcmp(check_sa_, check_sb_) != 0) { \
            check_failed_++;                                                     \
            fprintf(stderr, "%s:%d: CHECK_STR_EQ failed: %s == %s (\"%s\" vs \"%s\")\n", \
                    __FILE__, __LINE__, #a, #b, check_sa_ ? check_sa_ : "(null)", \
                    check_sb_ ? check_sb_ : "(null)");                           \
        }                                                                        \
    } while (0)

static inline int check_report_(const char *file)
{
    fprintf(stderr, "%s: %d checks, %d failed\n", file, check_total_, check_failed_);
    return check_failed_ != 0;
}

#define TEST_MAIN_END() return check_report_(__FILE__)

#endif /* CHECK_H */
