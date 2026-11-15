/* SPDX-License-Identifier: MPL-2.0
 *
 * What the Rust library cannot run yet (docs/REESCRITURA.md): it has the
 * 68000 interpreter (RR1); the 68010 to 68060 and the FPU come with RR2,
 * the JIT with RR3. relevo_create() returns NULL for those, so the tests
 * that need them are skipped, not run.
 *
 * test_020.c and test_fpu.c call internal functions of the C library
 * (cache_write_cacr, fpx_*) and are not built against the Rust one: their
 * tests get empty stand-ins here, and are skipped.
 */
#include <stddef.h>

#include "tests.h"

#define RR2_FPU "needs the FPU (RR2)"
#define RR3_JIT "needs the JIT (RR3)"

/* test_fpu.c: the FPU and its software arithmetic (fpx) */
#define TESTS_FPU(X)                                                        \
    X(fpx_against_x87) X(fpx_against_x87_more) X(fpx_basic) X(fpx_int_rem)  \
    X(fpx_conversions) X(fpx_packed) X(fpx_constants)                       \
    X(fpx_transcendental) X(fpu_table_no_conflicts) X(fpu_reset_state)      \
    X(fpu_move_and_arith) X(fpu_operands) X(fpu_fmovem) X(fpu_conditions)   \
    X(fpu_exceptions) X(fpu_fsave_frestore) X(fpu_models)

#define STAND_IN(name) void test_##name(void) {}
TESTS_FPU(STAND_IN)

#define SKIP_FPU(name) { #name, RR2_FPU },

const test_skip tests_skipped[] = {
    TESTS_FPU(SKIP_FPU)
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
