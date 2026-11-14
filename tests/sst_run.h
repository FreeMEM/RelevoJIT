/* SPDX-License-Identifier: MPL-2.0
 *
 * Runs one SingleStepTests case on a 68000 and compares the result.
 */
#ifndef RELEVO_TEST_SST_RUN_H
#define RELEVO_TEST_SST_RUN_H

#include <stdbool.h>
#include <stddef.h>

#include "relevo.h"
#include "sst_load.h"

typedef struct sst_env {
    relevo_cpu *cpu;
    uint8_t *mem;           /* 16 MB, the 68000's address space */
} sst_env;

bool sst_env_init(sst_env *env);
void sst_env_free(sst_env *env);

/* Loads the initial state, runs one instruction and compares registers and
 * memory with the final state. Returns true if they match; if not, `why`
 * gets a short description of the differences. Memory is left clean for the
 * next test. */
bool sst_run_one(sst_env *env, const sst_test *t, char *why, size_t why_size);

#endif
