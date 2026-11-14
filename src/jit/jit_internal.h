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
#define JIT_LINK_SIZE   (2 * JIT_POOL_SIZE)     /* chained jumps */
#define JIT_IND_SIZE    4096        /* the cache of indirect jump targets */
#define JIT_CODE_SIZE   (32u << 20) /* executable memory per CPU */

/* The JIT's map of the 68k address space, in pages of 4 KB: what a
 * translated access may do directly. */
#define JIT_MAP_SHIFT   12
#define JIT_MAP_SIZE    (1u << (32 - JIT_MAP_SHIFT))
#define JMAP_NO_READ    0x01u       /* not RAM or ROM */
#define JMAP_NO_WRITE   0x02u       /* not RAM */
#define JMAP_CODE       0x04u       /* RAM with translated code */
/* Translated code is tracked in chunks of 64 bytes of each 4 KB page. */
#define JIT_CHUNK_SHIFT 6

/* A translation: the code for the 68k instructions in [start, start+len)
 * (masked addresses), entered when the PC is `pc`. */
typedef struct jit_block {
    struct jit_block *next;         /* in the bucket, or free */
    struct jit_block *page_next;    /* in the list of its first 4 KB page */
    uint32_t pc;
    uint32_t start, len;
    const uint8_t *code;            /* NULL: run the interpreter here */
    uint32_t links;                 /* jumps chained to it (jit_link) */
    uint16_t ninsns;
    bool cut;                       /* shortened by the budget */
    bool rom;                       /* only ROM pages */
    bool listed;                    /* in a page list */
} jit_block;

/* A jump of some block patched to go straight to another block. */
typedef struct jit_link {
    uint32_t next;                  /* the target's next link; 0 ends */
    uint32_t site;                  /* the jump's rel32, in the code memory */
} jit_link;

/* An entry of the cache of indirect jump targets (RTS, JMP (An)): the
 * translated code for a PC. Translated code reads it (see translate.c). */
typedef struct jit_ind {
    uint32_t pc;                    /* 1 (odd): empty */
    uint32_t pad;
    const uint8_t *code;
} jit_ind;

struct jit_state {
    jit_ind ind[JIT_IND_SIZE];
    uint8_t map[JIT_MAP_SIZE];      /* JMAP_* per 4 KB page */
    codemem mem;
    size_t used;                    /* bytes of mem in use */
    const uint8_t *enter;           /* the trampoline into a block (RX) */
    size_t exit_off;                /* the common exit, in mem */
    size_t check_off;               /* the access check stub, in mem */
    size_t base_used;               /* what flushes keep (the trampoline) */
    bool has_movbe, movbe;
    bool force_interp;              /* next instruction: interpreter */
    jit_block *hash[JIT_HASH_SIZE];
    jit_block *pool;
    unsigned pool_used;
    jit_block *free_list;
    jit_link *links;
    uint32_t links_used, links_free;
    /* Per 4 KB page of the memory block: the blocks that start there and
     * the chunks with translated code. */
    uint32_t npages;
    jit_block **page_head;
    uint64_t *page_chunks;
    unsigned write_depth;           /* codemem_begin_write() nesting */
    jit_stats stats;
    jit_insn insns[JIT_MAX_INSNS];
};

/* ---- The code generator (x86_64/translate.c) -------------------------- */

/* x86 registers a block can give to 68k registers. */
#define JIT_HOST_REGS 9

/* What a block returns to the dispatcher (the low 32 bits of
 * jit_backend_enter()'s result; regs.pc is set in every case). */
enum {
    JIT_EXIT_NORMAL = 0,            /* go on at regs.pc */
    JIT_EXIT_BAIL = 1,              /* the interpreter runs regs.pc */
    JIT_EXIT_CHAIN = 2              /* as NORMAL; the high 32 bits are the
                                     * offset of a jump to patch to the
                                     * block at regs.pc */
};

bool jit_backend_available(void);
bool jit_backend_has_movbe(void);
/* Writes the trampoline and the shared stubs at the start of the
 * executable memory. */
bool jit_backend_init(relevo_cpu *cpu, jit_state *j);
/* Translates j->insns[0, n) into j->mem at j->used. Returns the code to
 * enter (RX), or NULL if the memory is full. */
const uint8_t *jit_backend_translate(relevo_cpu *cpu, jit_state *j,
                                     unsigned n);
/* Runs a block (and those chained to it). */
uint64_t jit_backend_enter(relevo_cpu *cpu, jit_state *j, const uint8_t *code);

/* Called by translated code (through the check stub) for an access the
 * map does not clear at once: 1 if `size` bytes at the masked address
 * `addr` can be read (or written, `write`) directly in the memory block,
 * which for a write means RAM with no translated code. */
int jit_access_check(relevo_cpu *cpu, uint32_t addr, uint32_t size_write);

#endif
