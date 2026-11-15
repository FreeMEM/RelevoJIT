/* SPDX-License-Identifier: MPL-2.0
 *
 * Unit test runner: runs every test in tests.h and prints a summary.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "tests.h"

static unsigned failures_in_test;

void check_fail(const char *file, int line, const char *expr)
{
    printf("    %s:%d: CHECK(%s) failed\n", file, line, expr);
    failures_in_test++;
}

void check_fail_eq(const char *file, int line, const char *a, const char *b,
                   uint64_t va, uint64_t vb)
{
    printf("    %s:%d: %s == %s failed: 0x%" PRIX64 " != 0x%" PRIX64 "\n",
           file, line, a, b, va, vb);
    failures_in_test++;
}

typedef struct test_entry {
    const char *name;
    void (*fn)(void);
} test_entry;

#define ENTRY(name) { #name, test_##name },
static const test_entry tests[] = { TEST_LIST(ENTRY) };

static const char *skip_reason(const char *name)
{
    for (const test_skip *s = tests_skipped; s->name != NULL; s++)
        if (strcmp(s->name, name) == 0)
            return s->why;
    return NULL;
}

int main(void)
{
    unsigned failed = 0, skipped = 0, n = sizeof tests / sizeof tests[0];
    for (unsigned i = 0; i < n; i++) {
        const char *why = skip_reason(tests[i].name);
        if (why != NULL) {
            printf("%-28s SKIPPED: %s\n", tests[i].name, why);
            skipped++;
            continue;
        }
        failures_in_test = 0;
        tests[i].fn();
        printf("%-28s %s\n", tests[i].name, failures_in_test ? "FAIL" : "ok");
        failed += failures_in_test != 0;
    }
    if (skipped != 0)
        printf("%u/%u tests passed, %u SKIPPED\n", n - skipped - failed,
               n - skipped, skipped);
    else
        printf("%u/%u tests passed\n", n - failed, n);
    return failed != 0;
}
