/* SPDX-License-Identifier: MPL-2.0
 *
 * A rough speed measure: the same 68k programs on the interpreter and on
 * the JIT, in millions of 68k instructions per second.
 *
 *   bench [-m 68000|68020|68040]
 *
 * The programs (assembled with vasm or GNU as; the source is in the
 * comments): an arithmetic loop, a memory copy, a loop over bytes with
 * compares, branches and calls, a loop with floating point (with a 68882;
 * not on the 68000) and one that reads and writes a host page. Each ends
 * with an RTS to 0. The final registers of both engines must be equal.
 */
#define _POSIX_C_SOURCE 199309L

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "relevo.h"

#define MEM_SIZE 0x100000u
#define CODE     0x1000u
#define STACK    0x8000u

typedef struct program {
    const char *name;
    const uint16_t *words;
    size_t nwords;
    bool fpu;           /* needs a 68882 */
} program;

#define HOST_PAGE 0xD0000u

/*      moveq   #0,d1
 *      move.l  #3000000,d0
 * .loop: add.l d0,d1
 *      eor.l   d1,d2
 *      subq.l  #1,d0
 *      bne.s   .loop
 *      rts                                                                 */
static const uint16_t arith[] = {
    0x7200, 0x203C, 0x002D, 0xC6C0, 0xD280, 0xB382, 0x5380, 0x66F8, 0x4E75
};

/*      move.l  #2000,d7
 * .outer: lea  $20000,a0
 *      lea     $30000,a1
 *      move.w  #1023,d0
 * .copy: move.l (a0)+,(a1)+
 *      dbra    d0,.copy
 *      subq.l  #1,d7
 *      bne.s   .outer
 *      rts                                                                 */
static const uint16_t copy[] = {
    0x2E3C, 0x0000, 0x07D0, 0x41F9, 0x0002, 0x0000, 0x43F9, 0x0003, 0x0000,
    0x303C, 0x03FF, 0x22D8, 0x51C8, 0xFFFC, 0x5387, 0x66E6, 0x4E75
};

/*      move.l  #6000,d7
 * .outer: lea  $20000,a0
 *      move.w  #255,d6
 *      moveq   #0,d0
 * .inner: move.b (a0)+,d1
 *      cmp.b   #$80,d1
 *      bcs.s   .low
 *      add.w   d1,d0
 *      bra.s   .next
 * .low: sub.w  d1,d0
 * .next: bsr.s mix
 *      dbra    d6,.inner
 *      subq.l  #1,d7
 *      bne.s   .outer
 *      rts
 * mix: eor.w   d0,d2
 *      and.w   #$7fff,d2
 *      or.w    d0,d3
 *      tst.w   d3
 *      beq.s   .z
 *      addq.w  #1,d4
 * .z:  rts                                                                 */
static const uint16_t bytes[] = {
    0x2E3C, 0x0000, 0x1770, 0x41F9, 0x0002, 0x0000, 0x3C3C, 0x00FF, 0x7000,
    0x1218, 0xB23C, 0x0080, 0x6504, 0xD041, 0x6002, 0x9041, 0x610A, 0x51CE,
    0xFFEE, 0x5387, 0x66DC, 0x4E75, 0xB142, 0xC47C, 0x7FFF, 0x8640, 0x4A43,
    0x6702, 0x5244, 0x4E75
};

/*      move.l  #4000,d7
 *      fmove.l #3,fp1
 *      fdiv.l  #7,fp1
 *      fmove.l #100000,fp3
 *      fmove.l #0,fp2
 * .outer: lea  $20000,a0
 *      move.w  #255,d0
 * .inner: fmove.l (a0)+,fp0
 *      fmul.x  fp1,fp0
 *      fadd.x  fp0,fp2
 *      fcmp.x  fp3,fp2
 *      fblt    .skip
 *      fsub.x  fp3,fp2
 * .skip: dbra  d0,.inner
 *      fmove.d fp2,(a0)
 *      subq.l  #1,d7
 *      bne.s   .outer
 *      rts                                                                 */
static const uint16_t fp[] = {
    0x2E3C, 0x0000, 0x0FA0, 0xF23C, 0x4080, 0x0000, 0x0003, 0xF23C, 0x40A0,
    0x0000, 0x0007, 0xF23C, 0x4180, 0x0001, 0x86A0, 0xF23C, 0x4100, 0x0000,
    0x0000, 0x41F9, 0x0002, 0x0000, 0x303C, 0x00FF, 0xF218, 0x4000, 0xF200,
    0x0423, 0xF200, 0x0122, 0xF200, 0x0D38, 0xF294, 0x0006, 0xF200, 0x0D28,
    0x51C8, 0xFFE6, 0xF210, 0x7500, 0x5387, 0x66D2, 0x4E75
};

/* A5 points into a host page.
 *      move.l  #2000000,d0
 *      moveq   #0,d2
 * .loop: move.w d0,(a5)
 *      move.w  (a5),d1
 *      add.w   d1,d2
 *      subq.l  #1,d0
 *      bne.s   .loop
 *      rts                                                                 */
static const uint16_t host[] = {
    0x203C, 0x001E, 0x8480, 0x7400, 0x3A80, 0x3215, 0xD441, 0x5380, 0x66F6,
    0x4E75
};

static const program programs[] = {
    { "arith", arith, sizeof arith / sizeof *arith, false },
    { "copy", copy, sizeof copy / sizeof *copy, false },
    { "bytes", bytes, sizeof bytes / sizeof *bytes, false },
    { "fp", fp, sizeof fp / sizeof *fp, true },
    { "host", host, sizeof host / sizeof *host, false },
};

/* The host page: a register that gives back what was written, plus 1. */
static uint32_t host_value;

static uint32_t host_read(void *ctx, uint32_t addr, unsigned size)
{
    (void)ctx;
    (void)addr;
    (void)size;
    return host_value + 1;
}

static void host_write(void *ctx, uint32_t addr, uint32_t v, unsigned size)
{
    (void)ctx;
    (void)addr;
    (void)size;
    host_value = v;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct result {
    double seconds;
    uint64_t insns;
    relevo_regs regs;
    bool ok;
} result;

static result run(const program *p, relevo_model model, relevo_engine engine)
{
    result res;
    memset(&res, 0, sizeof res);
    uint8_t *mem = calloc(1, MEM_SIZE);
    if (mem == NULL)
        return res;
    for (size_t i = 0; i < p->nwords; i++) {
        mem[CODE + 2 * i] = (uint8_t)(p->words[i] >> 8);
        mem[CODE + 2 * i + 1] = (uint8_t)p->words[i];
    }
    uint32_t x = 12345;
    for (uint32_t a = 0x20000; a < 0x21000; a++) {
        x = x * 1103515245u + 12345u;
        mem[a] = (uint8_t)(x >> 16);
    }
    relevo_config cfg = {0};
    cfg.model = model;
    if (p->fpu)
        cfg.fpu = model == RELEVO_68040 ? RELEVO_FPU_INTERNAL
                                        : RELEVO_FPU_68882;
    cfg.engine = engine;
    cfg.memory = mem;
    cfg.memory_size = MEM_SIZE;
    cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
    cfg.host.read = host_read;
    cfg.host.write = host_write;
    relevo_cpu *c = relevo_create(&cfg);
    if (c == NULL) {
        free(mem);
        return res;
    }
    relevo_map(c, HOST_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST);
    host_value = 0;
    relevo_regs *r = relevo_get_regs(c);
    r->a[7] = STACK;            /* the return address there is 0 */
    r->a[5] = HOST_PAGE + 0x100;
    r->pc = CODE;
    double t0 = now();
    relevo_status st = relevo_run(c, UINT64_MAX);
    res.seconds = now() - t0;
    res.insns = relevo_instructions(c);
    res.regs = *r;
    res.ok = st == RELEVO_EXIT_PC_ZERO;
    relevo_destroy(c);
    free(mem);
    return res;
}

int main(int argc, char **argv)
{
    relevo_model model = RELEVO_68020;
    if (argc == 3 && strcmp(argv[1], "-m") == 0) {
        if (strcmp(argv[2], "68000") == 0)
            model = RELEVO_68000;
        else if (strcmp(argv[2], "68040") == 0)
            model = RELEVO_68040;
    }
    int status = 0;
    printf("%-8s %12s %10s %10s %8s\n", "program", "instructions",
           "interp", "jit", "speedup");
    for (size_t i = 0; i < sizeof programs / sizeof *programs; i++) {
        const program *p = &programs[i];
        if (p->fpu && model == RELEVO_68000)
            continue;
        result a = run(p, model, RELEVO_ENGINE_INTERP);
        result b = run(p, model, RELEVO_ENGINE_JIT);
        if (!a.ok) {
            printf("%-8s the interpreter did not finish\n", p->name);
            status = 1;
            continue;
        }
        double mi = (double)a.insns / a.seconds / 1e6;
        if (!b.ok && b.insns == 0) {
            printf("%-8s %12" PRIu64 " %6.0f MIPS %10s\n", p->name, a.insns, mi,
                   "no JIT");
            continue;
        }
        bool same = b.ok && a.insns == b.insns
                 && memcmp(a.regs.d, b.regs.d, sizeof a.regs.d) == 0
                 && memcmp(a.regs.a, b.regs.a, sizeof a.regs.a) == 0
                 && a.regs.sr == b.regs.sr
                 && a.regs.fpsr == b.regs.fpsr;
        /* field by field: relevo_fpreg has padding */
        for (int k = 0; k < 8; k++)
            same = same && a.regs.fp[k].mantissa == b.regs.fp[k].mantissa
                && a.regs.fp[k].sign_exponent == b.regs.fp[k].sign_exponent;
        double mj = (double)b.insns / b.seconds / 1e6;
        printf("%-8s %12" PRIu64 " %6.0f MIPS %6.0f MIPS %7.1fx%s\n",
               p->name, a.insns, mi, mj, mj / mi,
               same ? "" : "  DIFFERENT RESULTS");
        if (!same)
            status = 1;
    }
    return status;
}
