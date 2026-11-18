/*
 * hal_test.h — Halcyon's tiny test harness
 *
 * Each tests/test_*.c is a standalone program with its own main(); it
 * prints one line per failed check and returns nonzero when anything
 * failed. `make test` runs them all and stops on the first failure.
 * Headless tests must not open a window: they exercise Halcyon's
 * offscreen surfaces, which need no display connection.
 */

#ifndef HAL_TEST_H
#define HAL_TEST_H

#include <stdio.h>

static int hal_test_checks = 0;
static int hal_test_failed = 0;

#define HAL_TEST_CHECK(cond)                                            \
    do {                                                                \
        hal_test_checks++;                                              \
        if (!(cond)) {                                                  \
            hal_test_failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
        }                                                               \
    } while (0)

#define HAL_TEST_CHECK_EQ_I(actual, expected)                           \
    do {                                                                \
        long long a_ = (long long)(actual);                             \
        long long e_ = (long long)(expected);                           \
        hal_test_checks++;                                              \
        if (a_ != e_) {                                                 \
            hal_test_failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, wanted %lld\n",    \
                    __FILE__, __LINE__, #actual, a_, e_);               \
        }                                                               \
    } while (0)

#define HAL_TEST_CHECK_EQ_F(actual, expected, tol)                      \
    do {                                                                \
        double a_ = (double)(actual);                                   \
        double e_ = (double)(expected);                                 \
        hal_test_checks++;                                              \
        if (a_ > e_ + (tol) || a_ < e_ - (tol)) {                       \
            hal_test_failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %g, wanted %g +/- %g\n", \
                    __FILE__, __LINE__, #actual, a_, e_, (double)(tol));\
        }                                                               \
    } while (0)

#define HAL_TEST_SUMMARY(name)                                          \
    do {                                                                \
        printf("%s: %d checks, %d failed\n", (name), hal_test_checks,   \
               hal_test_failed);                                        \
        return hal_test_failed ? 1 : 0;                                 \
    } while (0)

#endif /* HAL_TEST_H */
