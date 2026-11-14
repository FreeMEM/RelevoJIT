/* SPDX-License-Identifier: MPL-2.0
 *
 * A minimal test harness. A test is a void function; CHECK and CHECK_EQ
 * report failures and let the test go on. The list of tests is in tests.h.
 */
#ifndef RELEVO_TEST_CHECK_H
#define RELEVO_TEST_CHECK_H

#include <stdint.h>

void check_fail(const char *file, int line, const char *expr);
void check_fail_eq(const char *file, int line, const char *a, const char *b,
                   uint64_t va, uint64_t vb);

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
            check_fail(__FILE__, __LINE__, #cond);                        \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        uint64_t check_a_ = (uint64_t)(a), check_b_ = (uint64_t)(b);      \
        if (check_a_ != check_b_)                                         \
            check_fail_eq(__FILE__, __LINE__, #a, #b, check_a_, check_b_); \
    } while (0)

#endif
