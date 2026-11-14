/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT engine, as the rest of the library sees it. Not part of the API.
 *
 * The dispatcher (jit.c) runs translated blocks and falls back to the
 * interpreter, one instruction at a time, for what it does not translate.
 * Translated blocks never call C and never fault: before the first side
 * effect of an instruction they check every memory access, and if one is
 * not a plain access to RAM (or ROM, for reads) they leave the block with
 * the state from before that instruction, which the interpreter then runs.
 * So exceptions, host pages and the 68000's fault frames all come from the
 * interpreter, exactly as without the JIT.
 */
#ifndef RELEVO_JIT_H
#define RELEVO_JIT_H

#include <stdbool.h>
#include <stdint.h>

#include "../cpu.h"

typedef struct jit_state jit_state;

/* Whether this build and this machine can run the JIT. */
bool jit_available(void);

/* Sets up cpu->jit. Returns false if there is no executable memory. */
bool jit_create(relevo_cpu *cpu);
void jit_destroy(relevo_cpu *cpu);

/* The run loop of the JIT engine: like the interpreter's, it runs until
 * the budget is spent or another reason to stop comes up. Called from
 * relevo_run(), inside its setjmp(). */
relevo_status jit_run_loop(relevo_cpu *cpu);

/* Drops the translations of code in [address, address + size). */
void jit_invalidate(relevo_cpu *cpu, uint32_t address, uint32_t size);
/* Drops every translation (the page map changed). */
void jit_flush(relevo_cpu *cpu);

/* ---- For the tests --------------------------------------------------- */

typedef struct jit_stats {
    uint64_t blocks;        /* blocks translated */
    uint64_t insns;         /* instructions translated */
    uint64_t runs;          /* blocks entered */
    uint64_t bails;         /* left a block for the interpreter */
    uint64_t interp;        /* instructions run by the interpreter */
    uint64_t flushes;       /* whole cache flushes */
} jit_stats;

void jit_get_stats(const relevo_cpu *cpu, jit_stats *out);
/* Uses MOVBE or not (when the CPU has it): to test both paths. Flushes. */
void jit_set_movbe(relevo_cpu *cpu, bool on);
/* Whether the first instruction at `pc` would be translated. */
bool jit_translatable(relevo_cpu *cpu, uint32_t pc);

#endif /* RELEVO_JIT_H */
