/* SPDX-License-Identifier: MPL-2.0
 *
 * What the JIT's dispatcher (jit.c) and its code generator (x86_64/) share.
 */
#ifndef RELEVO_JIT_INTERNAL_H
#define RELEVO_JIT_INTERNAL_H

#include "codemem.h"
#include "decode.h"
#include "jit.h"

#define JIT_MAX_INSNS   64          /* instructions per block */
#define JIT_HASH_SIZE   4096        /* buckets, by 68k address */
#define JIT_POOL_SIZE   65536       /* block descriptors */
#define JIT_CODE_SIZE   (32u << 20) /* executable memory per CPU */

/* A translation: the code for the 68k instructions in [start, start+len)
 * (masked addresses), entered when the PC is `pc`. */
typedef struct jit_block {
    struct jit_block *next;         /* in the bucket, or free */
    uint32_t pc;
    uint32_t start, len;
    uint64_t hash;                  /* of the 68k code it was made from */
    const uint8_t *code;            /* NULL: run the interpreter here */
    uint16_t ninsns;
    bool cut;                       /* shortened by the budget */
    bool rom;                       /* only ROM pages: not verified */
} jit_block;

struct jit_state {
    codemem mem;
    size_t used;                    /* bytes of mem in use */
    const uint8_t *enter;           /* the trampoline into a block (RX) */
    size_t exit_off;                /* where blocks jump to leave */
    size_t base_used;               /* what flushes keep (the trampoline) */
    bool has_movbe, movbe;
    bool force_interp;              /* next instruction: interpreter */
    jit_block *hash[JIT_HASH_SIZE];
    jit_block *pool;
    unsigned pool_used;
    jit_block *free_list;
    jit_stats stats;
    jit_insn insns[JIT_MAX_INSNS];
};

/* ---- The code generator (x86_64/translate.c) -------------------------- */

/* x86 registers a block can give to 68k registers. */
#define JIT_HOST_REGS 9

bool jit_backend_available(void);
bool jit_backend_has_movbe(void);
/* Writes the trampoline at the start of the executable memory. */
bool jit_backend_init(relevo_cpu *cpu, jit_state *j);
/* Translates j->insns[0, n) into j->mem at j->used. Returns the code to
 * enter (RX), or NULL if the memory is full. */
const uint8_t *jit_backend_translate(relevo_cpu *cpu, jit_state *j,
                                     unsigned n);
/* Runs a block: 0 when it ended normally, 1 when it left for the
 * interpreter (regs.pc is the instruction to interpret). */
int jit_backend_enter(relevo_cpu *cpu, jit_state *j, const uint8_t *code);

#endif
