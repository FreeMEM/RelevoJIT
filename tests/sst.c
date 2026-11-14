/* SPDX-License-Identifier: MPL-2.0
 *
 * SingleStepTests/m68000 runner.
 *
 *   sst [-v N] FILE.json...
 *
 * Runs every test in each file and prints passed/total per file (one file
 * per instruction group) and a total. -v N prints the first N failures of
 * each file. Exits with 1 if any test failed. Get the files with
 * tools/datos.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sst_run.h"

static const char *base_name(const char *path, char *buf, size_t size)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    snprintf(buf, size, "%s", s);
    char *dot = strstr(buf, ".json");
    if (dot)
        *dot = 0;
    return buf;
}

int main(int argc, char **argv)
{
    long verbose = 0;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-v") == 0) {
        verbose = strtol(argv[2], NULL, 10);
        first = 3;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: sst [-v N] FILE.json...\n");
        return 2;
    }

    sst_env env;
    if (!sst_env_init(&env)) {
        fprintf(stderr, "sst: cannot create the CPU\n");
        return 2;
    }
    sst_test t;
    memset(&t, 0, sizeof t);
    unsigned long total = 0, passed = 0, files_ok = 0, files = 0;
    int status = 0;

    for (int i = first; i < argc; i++) {
        sst_reader rd;
        char name[64];
        base_name(argv[i], name, sizeof name);
        if (!sst_open(&rd, argv[i])) {
            status = 2;
            continue;
        }
        unsigned long n = 0, ok = 0;
        long shown = 0;
        int res;
        while ((res = sst_next(&rd, &t)) == 1) {
            char why[512];
            n++;
            if (sst_run_one(&env, &t, why, sizeof why)) {
                ok++;
            } else if (shown < verbose) {
                printf("  FAIL %s: %s\n", t.name, why);
                shown++;
            }
        }
        if (res < 0) {
            fprintf(stderr, "%s: bad JSON at byte %zu: %s\n", argv[i],
                    rd.j.err_offset, rd.j.err);
            status = 2;
        }
        sst_close(&rd);
        printf("%-16s %5lu/%5lu%s\n", name, ok, n, ok == n ? "" : "  FAIL");
        total += n;
        passed += ok;
        files++;
        files_ok += ok == n && n > 0;
    }

    printf("total: %lu/%lu tests, %lu/%lu files\n", passed, total, files_ok,
           files);
    sst_test_free(&t);
    sst_env_free(&env);
    if (status == 0 && passed != total)
        status = 1;
    return status;
}
