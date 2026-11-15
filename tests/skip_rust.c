/* SPDX-License-Identifier: MPL-2.0
 *
 * What the Rust library cannot run yet (docs/REESCRITURA.md): it has the
 * interpreter, 68000 to 68060 and the FPU (RR1 and RR2); the JIT comes with
 * RR3. relevo_create() returns NULL for the JIT engine, so the tests that
 * need it are skipped, not run.
 */
#include <stddef.h>

#include "tests.h"

#define RR3_JIT "needs the JIT (RR3)"

const test_skip tests_skipped[] = {
    /* test_jit.c */
    { "jit_engine_available", RR3_JIT },
    { "jit_loop", RR3_JIT },
    { "jit_budget", RR3_JIT },
    { "jit_code_change", RR3_JIT },
    { "jit_invalidate_rom", RR3_JIT },
    { "jit_self_modifying", RR3_JIT },
    { "jit_host_page", RR3_JIT },
    { "jit_address_error", RR3_JIT },
    { "jit_interrupt", RR3_JIT },
    { "jit_calls", RR3_JIT },
    { "jit_chaining", RR3_JIT },
    { "jit_indirect", RR3_JIT },
    { "jit_budget_chained", RR3_JIT },
    { "jit_write_near_code", RR3_JIT },
    { "jit_write_other_block", RR3_JIT },
    { "jit_cpush", RR3_JIT },
    { "jit_translated", RR3_JIT },
    { "jit_host_patches_block", RR3_JIT },
    { NULL, NULL }
};
