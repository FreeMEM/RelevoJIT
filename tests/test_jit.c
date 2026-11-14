/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT engine: what the random differential test (tests/jitdiff.c) does
 * not pin down. Each test is skipped where there is no JIT.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "relevo.h"
#include "tests.h"
#include "../src/jit/jit.h"

#define MEM_SIZE 0x100000u
#define CODE     0x1000u
#define STACK    0x8000u

typedef struct jt {
    relevo_cpu *cpu;
    uint8_t *mem;
    relevo_regs *r;
    unsigned writes;
    uint32_t last_addr, last_value;
} jt;

static void put16(uint8_t *m, uint32_t a, uint32_t v)
{
    m[a] = (uint8_t)(v >> 8);
    m[a + 1] = (uint8_t)v;
}

static void put32(uint8_t *m, uint32_t a, uint32_t v)
{
    put16(m, a, v >> 16);
    put16(m, a + 2, v);
}

static uint32_t get32(const uint8_t *m, uint32_t a)
{
    return (uint32_t)m[a] << 24 | (uint32_t)m[a + 1] << 16
         | (uint32_t)m[a + 2] << 8 | m[a + 3];
}

static void put_code(uint8_t *m, uint32_t a, const uint16_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put16(m, a + 2 * (uint32_t)i, w[i]);
}

static void host_write(void *ctx, uint32_t addr, uint32_t v, unsigned size)
{
    jt *t = ctx;
    (void)size;
    t->writes++;
    t->last_addr = addr;
    t->last_value = v;
}

/* A CPU with 1 MB of RAM, the stack at STACK with 0 as return address and
 * the PC at CODE. Returns false (and the test does nothing) without JIT. */
static bool jt_init(jt *t, relevo_model model, relevo_engine engine)
{
    memset(t, 0, sizeof *t);
    t->mem = calloc(1, MEM_SIZE);
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.engine = engine;
    cfg.memory = t->mem;
    cfg.memory_size = MEM_SIZE;
    cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
    cfg.host.ctx = t;
    cfg.host.write = host_write;
    t->cpu = relevo_create(&cfg);
    if (t->cpu == NULL) {
        free(t->mem);
        return false;
    }
    t->r = relevo_get_regs(t->cpu);
    t->r->a[7] = STACK;
    t->r->pc = CODE;
    return true;
}

static void jt_free(jt *t)
{
    relevo_destroy(t->cpu);
    free(t->mem);
}

void test_jit_engine_available(void)
{
    jt t;
    bool ok = jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(ok);
#endif
    if (ok)
        jt_free(&t);
}

/* A loop that branches to its own start runs inside one block. */
void test_jit_loop(void)
{
    /* moveq #0,d1; move.l #1000,d0; .l: add.l d0,d1; subq.l #1,d0;
     * bne.s .l; rts */
    static const uint16_t code[] = { 0x7200, 0x203C, 0x0000, 0x03E8, 0xD280,
                                     0x5380, 0x66FA, 0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 8);
    CHECK_EQ(relevo_run(t.cpu, 100000), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[1], 500500);
    CHECK_EQ(t.r->d[0], 0);
    CHECK_EQ(t.r->sr & 0x1F, RELEVO_SR_Z);
    CHECK_EQ(relevo_instructions(t.cpu), 2 + 3 * 1000 + 1);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK(s.blocks >= 2);
    CHECK(s.runs < 10);
    jt_free(&t);
}

/* The budget is exact, also inside a looping block. */
void test_jit_budget(void)
{
    static const uint16_t code[] = { 0x5280, 0x60FC };  /* addq.l #1,d0; bra */
    jt t;
    if (!jt_init(&t, RELEVO_68020, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 2);
    CHECK_EQ(relevo_run(t.cpu, 1001), RELEVO_BUDGET);
    CHECK_EQ(relevo_instructions(t.cpu), 1001);
    CHECK_EQ(t.r->d[0], 501);
    CHECK_EQ(t.r->pc, CODE + 2);
    CHECK_EQ(relevo_run(t.cpu, 3), RELEVO_BUDGET);
    CHECK_EQ(relevo_instructions(t.cpu), 1004);
    CHECK_EQ(t.r->d[0], 502);
    CHECK_EQ(t.r->pc, CODE);
    jt_free(&t);
}

/* Code the host changes between runs is translated again once it says so
 * (relevo_invalidate()), or at once if it writes through relevo_write(). */
void test_jit_code_change(void)
{
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    static const uint16_t code[] = { 0x7001, 0x4E75 };  /* moveq #1,d0; rts */
    put_code(t.mem, CODE, code, 2);
    put32(t.mem, STACK, 0);
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 1);
    put16(t.mem, CODE, 0x7002);                         /* moveq #2,d0 */
    relevo_invalidate(t.cpu, CODE, 2);
    t.r->pc = CODE;
    t.r->a[7] = STACK;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 2);
    relevo_write(t.cpu, CODE, 0x7003, 2);               /* moveq #3,d0 */
    t.r->pc = CODE;
    t.r->a[7] = STACK;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 3);
    jt_free(&t);
}

/* Code in ROM is not hashed again: relevo_invalidate() drops it. */
void test_jit_invalidate_rom(void)
{
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    static const uint16_t code[] = { 0x7001, 0x4E75 };
    put_code(t.mem, 0x50000, code, 2);
    CHECK_EQ(relevo_map(t.cpu, 0x50000, RELEVO_PAGE_SIZE, RELEVO_PAGE_ROM),
             0);
    t.r->pc = 0x50000;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 1);
    put16(t.mem, 0x50000, 0x7002);
    relevo_invalidate(t.cpu, 0x50000, 2);
    t.r->pc = 0x50000;
    t.r->a[7] = STACK;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 2);
    jt_free(&t);
}

/* A write into the code of the running block: the next instruction is the
 * new one, as in the interpreter. */
void test_jit_self_modifying(void)
{
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    /* move.w #$7005,$1008; nop; nop; moveq #1,d0; rts */
    static const uint16_t code[] = { 0x31FC, 0x7005, 0x1008, 0x4E71,
                                     0x7001, 0x4E75 };
    put_code(t.mem, CODE, code, 6);
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 5);
    jt_free(&t);
}

/* An access to a host page inside a block goes to the host function once,
 * and the block's other instructions still run. */
void test_jit_host_page(void)
{
    jt t;
    if (!jt_init(&t, RELEVO_68020, RELEVO_ENGINE_JIT))
        return;
    CHECK_EQ(relevo_map(t.cpu, 0xD0000, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST),
             0);
    /* moveq #7,d0; move.l d0,(a0); addq.l #1,d0; move.l d0,(a1); rts */
    static const uint16_t code[] = { 0x7007, 0x2080, 0x5280, 0x2280,
                                     0x4E75 };
    put_code(t.mem, CODE, code, 5);
    t.r->a[0] = 0xD0010;
    t.r->a[1] = 0x20000;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.writes, 1);
    CHECK_EQ(t.last_addr, 0xD0010);
    CHECK_EQ(t.last_value, 7);
    CHECK_EQ(get32(t.mem, 0x20000), 8);
    CHECK_EQ(relevo_instructions(t.cpu), 5);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK_EQ(s.bails, 1);
    jt_free(&t);
}

/* An address error inside a block gives the interpreter's frame. */
void test_jit_address_error(void)
{
    /* moveq #3,d1; move.w (a0),d0; rts */
    static const uint16_t code[] = { 0x7203, 0x3010, 0x4E75 };
    jt a, b;
    if (!jt_init(&b, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    CHECK(jt_init(&a, RELEVO_68000, RELEVO_ENGINE_INTERP));
    jt *both[2] = { &a, &b };
    for (int i = 0; i < 2; i++) {
        jt *t = both[i];
        put_code(t->mem, CODE, code, 3);
        put32(t->mem, 3 * 4, 0x2000);           /* address error vector */
        put16(t->mem, 0x2000, 0x4E71);
        t->r->a[0] = 0x30001;
        relevo_run(t->cpu, 3);
    }
    CHECK_EQ(b.r->pc, a.r->pc);
    CHECK_EQ(b.r->pc, 0x2002);
    CHECK_EQ(b.r->d[1], 3);
    CHECK_EQ(b.r->a[7], a.r->a[7]);
    CHECK_EQ(b.r->sr, a.r->sr);
    CHECK(memcmp(a.mem + STACK - 14, b.mem + STACK - 14, 14) == 0);
    jt_free(&a);
    jt_free(&b);
}

/* Interrupts are taken between blocks, also out of a looping one. */
void test_jit_interrupt(void)
{
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    put16(t.mem, CODE, 0x60FE);                         /* bra.s * */
    put32(t.mem, (24 + 3) * 4, 0x3000);                 /* level 3 */
    put16(t.mem, 0x3000, 0x60FE);
    relevo_set_sr(t.cpu, 0x2000);                       /* mask 0 */
    CHECK_EQ(relevo_run(t.cpu, 1000), RELEVO_BUDGET);
    CHECK_EQ(t.r->pc, CODE);
    relevo_set_ipl(t.cpu, 3);
    CHECK_EQ(relevo_run(t.cpu, 10), RELEVO_BUDGET);
    CHECK_EQ(t.r->pc, 0x3000);
    CHECK_EQ(t.r->sr & RELEVO_SR_IPL, 0x0300);
    relevo_request_stop(t.cpu);
    CHECK_EQ(relevo_run(t.cpu, 10), RELEVO_HOST_STOP);
    jt_free(&t);
}

/* Subroutines: BSR, JSR and RTS through translated code. */
void test_jit_calls(void)
{
    /* moveq #0,d0; bsr.s f; jsr f(pc)... as jsr $1010; rts
     * f ($100C): addq.l #1,d0; rts                                         */
    static const uint16_t code[] = { 0x7000, 0x6108, 0x4EB9, 0x0000, 0x100C,
                                     0x4E75, 0x5280, 0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 8);
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 2);
    CHECK_EQ(t.r->a[7], STACK + 4);
    CHECK_EQ(relevo_instructions(t.cpu), 8);
    jt_free(&t);
}

/* Two blocks branching to each other are chained: they run without
 * going back to the dispatcher. */
void test_jit_chaining(void)
{
    /* moveq #0,d0; move.l #10000,d1; .a: addq.l #1,d0; bra.s .b; nop;
     * .b: subq.l #1,d1; bne.s .a; rts */
    static const uint16_t code[] = { 0x7000, 0x223C, 0x0000, 0x2710, 0x5280,
                                     0x6002, 0x4E71, 0x5381, 0x66F6,
                                     0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 10);
    CHECK_EQ(relevo_run(t.cpu, 1000000), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 10000);
    CHECK_EQ(t.r->d[1], 0);
    CHECK_EQ(relevo_instructions(t.cpu), 2 + 4 * 10000 + 1);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK(s.links >= 2);
    CHECK(s.runs < 20);
    jt_free(&t);
}

/* RTS finds its target in the cache of indirect jumps. */
void test_jit_indirect(void)
{
    /* move.l #1000,d1; .l: bsr.s f; subq.l #1,d1; bne.s .l; rts; nop;
     * f: addq.l #2,d0; rts */
    static const uint16_t code[] = { 0x223C, 0x0000, 0x03E8, 0x6108, 0x5381,
                                     0x66FA, 0x4E75, 0x4E71, 0x5480,
                                     0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68020, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 10);
    CHECK_EQ(relevo_run(t.cpu, 1000000), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[0], 2000);
    CHECK_EQ(t.r->a[7], STACK + 4);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK(s.runs < 20);
    jt_free(&t);
}

/* The budget stays exact through chained blocks: the same state as the
 * interpreter after any number of instructions. */
void test_jit_budget_chained(void)
{
    static const uint16_t code[] = { 0x7000, 0x223C, 0x0000, 0x2710, 0x5280,
                                     0x6002, 0x4E71, 0x5381, 0x66F6,
                                     0x4E75 };
    jt a, b;
    if (!jt_init(&b, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    CHECK(jt_init(&a, RELEVO_68000, RELEVO_ENGINE_INTERP));
    put_code(a.mem, CODE, code, 10);
    put_code(b.mem, CODE, code, 10);
    static const uint64_t budgets[] = { 1, 2, 3, 5, 7, 64, 65, 100, 1001, 3 };
    for (unsigned i = 0; i < sizeof budgets / sizeof *budgets; i++) {
        relevo_status sa = relevo_run(a.cpu, budgets[i]);
        relevo_status sb = relevo_run(b.cpu, budgets[i]);
        CHECK_EQ(sb, sa);
        CHECK_EQ(b.r->pc, a.r->pc);
        CHECK_EQ(b.r->d[0], a.r->d[0]);
        CHECK_EQ(b.r->d[1], a.r->d[1]);
        CHECK_EQ(b.r->sr, a.r->sr);
        CHECK_EQ(relevo_instructions(b.cpu), relevo_instructions(a.cpu));
    }
    jt_free(&a);
    jt_free(&b);
}

/* Writes to data next to the code (same page, same 64-byte chunk) run in
 * the block: only writes into translated code leave it. */
void test_jit_write_near_code(void)
{
    /* move.l #1000,d1; lea var(pc),a0; .l: addq.l #1,(a0); subq.l #1,d1;
     * bne.s .l; rts; nop; var: dc.l 0 */
    static const uint16_t code[] = { 0x223C, 0x0000, 0x03E8, 0x41FA, 0x000C,
                                     0x5290, 0x5381, 0x66FA, 0x4E75,
                                     0x4E71, 0x0000, 0x0000 };
    jt t;
    if (!jt_init(&t, RELEVO_68020, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 12);
    CHECK_EQ(relevo_run(t.cpu, 100000), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(get32(t.mem, CODE + 0x14), 1000);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK_EQ(s.bails, 0);
    CHECK_EQ(s.dropped, 0);
    jt_free(&t);
}

/* A block that writes into another block's code: the other one is
 * dropped and translated again. */
void test_jit_write_other_block(void)
{
    /* bsr.s f; move.w #$7205,$1012.w; bsr.s f; rts; nop; nop; nop;
     * f ($1012): moveq #1,d1; rts                                         */
    static const uint16_t code[] = { 0x6110, 0x31FC, 0x7205, 0x1012, 0x6108,
                                     0x4E75, 0x4E71, 0x4E71, 0x4E71,
                                     0x7201, 0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68000, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, CODE, code, 11);
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[1], 5);
    CHECK_EQ(relevo_instructions(t.cpu), 8);
    jit_stats s;
    jit_get_stats(t.cpu, &s);
    CHECK(s.dropped >= 1);
    jt_free(&t);
}

/* CPUSHA on the 68040 drops what the host changed behind the CPU's back. */
void test_jit_cpush(void)
{
    /* f ($2000): moveq #1,d1; rts. Run 1: jsr $2000; rts.
     * Run 2 ($1100): cpusha bc; jsr $2000; rts                            */
    static const uint16_t f[] = { 0x7201, 0x4E75 };
    static const uint16_t run1[] = { 0x4EB9, 0x0000, 0x2000, 0x4E75 };
    static const uint16_t run2[] = { 0xF4F8, 0x4EB9, 0x0000, 0x2000, 0x4E75 };
    jt t;
    if (!jt_init(&t, RELEVO_68040, RELEVO_ENGINE_JIT))
        return;
    put_code(t.mem, 0x2000, f, 2);
    put_code(t.mem, CODE, run1, 4);
    put_code(t.mem, 0x1100, run2, 5);
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[1], 1);
    put16(t.mem, 0x2000, 0x7207);                       /* moveq #7,d1 */
    t.r->pc = 0x1100;
    t.r->a[7] = STACK;
    CHECK_EQ(relevo_run(t.cpu, 100), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(t.r->d[1], 7);
    jt_free(&t);
}
