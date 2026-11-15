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

#define RR2_010 "needs the 68010 to 68060 (RR2)"
#define RR2_FPU "needs the FPU (RR2)"
#define RR3_JIT "needs the JIT (RR3)"

/* test_020.c: 68020 to 68040 instructions and addressing modes */
#define TESTS_020(X)                                                        \
    X(ea_020_brief_scaled) X(ea_020_full_no_indirect)                       \
    X(ea_020_memory_indirect) X(ea_020_pc_relative) X(ea_020_reserved)      \
    X(mul_l) X(div_l) X(extb_l) X(chk_l) X(cmp2) X(chk2) X(cas) X(cas2)     \
    X(trapcc) X(pack_unpk) X(bf_register) X(bf_memory) X(link_l)            \
    X(branch_long) X(cmpi_tst_020) X(move16) X(cinv_cpush) X(cacr)

/* test_fpu.c: the FPU and its software arithmetic (fpx) */
#define TESTS_FPU(X)                                                        \
    X(fpx_against_x87) X(fpx_against_x87_more) X(fpx_basic) X(fpx_int_rem)  \
    X(fpx_conversions) X(fpx_packed) X(fpx_constants)                       \
    X(fpx_transcendental) X(fpu_table_no_conflicts) X(fpu_reset_state)      \
    X(fpu_move_and_arith) X(fpu_operands) X(fpu_fmovem) X(fpu_conditions)   \
    X(fpu_exceptions) X(fpu_fsave_frestore) X(fpu_models)

#define STAND_IN(name) void test_##name(void) {}
TESTS_020(STAND_IN)
TESTS_FPU(STAND_IN)

#define SKIP_010(name) { #name, RR2_010 },
#define SKIP_FPU(name) { #name, RR2_FPU },

const test_skip tests_skipped[] = {
    /* test_api.c: these also create 68010 to 68060 CPUs */
    { "config_validation", RR2_010 },
    { "map_pages", RR2_010 },
    { "memory_page_crossing", RR2_010 },
    { "address_mask", RR2_010 },
    { "rts_to_zero_exits", RR2_010 },
    { "set_sr_switches_stacks", RR2_010 },
    { "illegal_exception_68010", RR2_010 },
    { "independent_cpus", RR2_010 },
    /* test_010.c */
    { "movec_68010", RR2_010 },
    { "move_from_sr_privileged", RR2_010 },
    { "rtd", RR2_010 },
    { "bus_error_frames", RR2_010 },
    { "frames_format2", RR2_010 },
    { "interrupt_master_stack", RR2_010 },
    { "format_error", RR2_010 },
    TESTS_020(SKIP_010)
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
