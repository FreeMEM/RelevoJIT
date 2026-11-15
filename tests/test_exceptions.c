/* SPDX-License-Identifier: MPL-2.0
 *
 * Tests of the 68000 exceptions that SingleStepTests does not cover:
 * interrupts, STOP, trace across instructions, bus errors, double faults,
 * RESET; and of the opcode table itself.
 */
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "check.h"
#include "tests.h"

#define MEM_SIZE 0x40000u   /* 256 KB; 0x40000 and up are unmapped */
#define CODE 0x400u
#define SSP 0x1000u
#define HANDLER 0x800u

static uint8_t mem[MEM_SIZE];
static unsigned resets;

static void count_reset(void *ctx)
{
    (void)ctx;
    resets++;
}

static void put16(uint32_t a, uint16_t v)
{
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}

static void put32(uint32_t a, uint32_t v)
{
    put16(a, (uint16_t)(v >> 16));
    put16(a + 2, (uint16_t)v);
}

static uint32_t get16(uint32_t a)
{
    return (uint32_t)mem[a] << 8 | mem[a + 1];
}

static uint32_t get32(uint32_t a)
{
    return get16(a) << 16 | get16(a + 2);
}

/* A 68000 in supervisor mode with interrupts open, running at CODE, every
 * vector pointing at HANDLER (NOPs), the code given as words. */
static relevo_cpu *setup(const uint16_t *code, size_t n)
{
    memset(mem, 0, sizeof mem);
    relevo_config cfg = {0};
    cfg.memory = mem;
    cfg.memory_size = sizeof mem;
    cfg.host.reset = count_reset;
    relevo_cpu *c = relevo_create(&cfg);
    put32(0, SSP);
    put32(4, CODE);
    for (unsigned v = 2; v < 64; v++)
        put32(v * 4, HANDLER);
    for (uint32_t a = HANDLER; a < HANDLER + 32; a += 2)
        put16(a, 0x4E71);
    for (size_t i = 0; i < n; i++)
        put16(CODE + 2 * (uint32_t)i, code[i]);
    relevo_reset(c);
    relevo_set_sr(c, 0x2000);
    return c;
}

void test_table_no_conflicts(void)
{
    relevo_handler *t = malloc(sizeof *t * 0x10000);
    CHECK(t != NULL);
    if (t == NULL)
        return;
    for (relevo_model m = RELEVO_68000; m <= RELEVO_68060; m++)
        CHECK_EQ(interp_fill_table(t, m, RELEVO_FPU_NONE), 0);
    free(t);
}

void test_address_error_frame(void)
{
    static const uint16_t code[] = { 0x3080 };     /* MOVE.W D0,(A0) */
    relevo_cpu *c = setup(code, 1);
    relevo_regs *r = relevo_get_regs(c);
    r->a[0] = 0x12345;
    r->d[0] = 0;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(r->a[7], SSP - 14);
    CHECK_EQ(get16(SSP - 14), 0x3085);      /* IR bits, write, FC 5 */
    CHECK_EQ(get32(SSP - 12), 0x12345);     /* the access address */
    CHECK_EQ(get16(SSP - 8), 0x3080);       /* IR */
    CHECK_EQ(get16(SSP - 6), 0x2004);       /* SR, with MOVE's Z */
    CHECK_EQ(get32(SSP - 4), CODE + 4);     /* after the next prefetch */
    relevo_destroy(c);
}

void test_bus_error_unmapped(void)
{
    static const uint16_t code[] = { 0x3010 };     /* MOVE.W (A0),D0 */
    relevo_cpu *c = setup(code, 1);
    relevo_regs *r = relevo_get_regs(c);
    r->a[0] = 0x100000;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(get32(2 * 4), HANDLER);
    CHECK_EQ(get16(SSP - 14), 0x3015);      /* read, FC 5 */
    CHECK_EQ(get32(SSP - 12), 0x100000);
    /* The host can still read there: 0, no fault. */
    CHECK_EQ(relevo_read(c, 0x100000, 2), 0);
    relevo_destroy(c);
}

void test_double_fault_halts(void)
{
    static const uint16_t code[] = { 0x3080 };     /* MOVE.W D0,(A0) */
    relevo_cpu *c = setup(code, 1);
    relevo_regs *r = relevo_get_regs(c);
    r->a[0] = 1;
    r->a[7] = SSP + 1;                      /* the frame cannot be pushed */
    CHECK_EQ(relevo_run(c, 10), RELEVO_HALTED);
    CHECK_EQ(relevo_run(c, 10), RELEVO_HALTED);
    relevo_reset(c);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    relevo_destroy(c);
}

/* The trace exception comes before the next instruction. */
void test_trace(void)
{
    static const uint16_t code[] = { 0x4E71, 0x4E71 };
    relevo_cpu *c = setup(code, 2);
    relevo_regs *r = relevo_get_regs(c);
    relevo_set_sr(c, 0xA000);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, CODE + 2);
    CHECK_EQ(r->a[7], SSP);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER + 2);           /* trace, then its first NOP */
    CHECK_EQ(r->sr, 0x2000);
    CHECK_EQ(get16(SSP - 6), 0xA000);
    CHECK_EQ(get32(SSP - 4), CODE + 2);
    relevo_destroy(c);
}

/* Illegal instructions are not traced. */
void test_trace_not_after_illegal(void)
{
    static const uint16_t code[] = { 0x4AFC };     /* ILLEGAL */
    relevo_cpu *c = setup(code, 1);
    relevo_regs *r = relevo_get_regs(c);
    relevo_set_sr(c, 0xA000);
    CHECK_EQ(relevo_run(c, 2), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER + 2);
    CHECK_EQ(r->a[7], SSP - 6);             /* one frame only */
    relevo_destroy(c);
}

void test_interrupt_autovector(void)
{
    static const uint16_t code[] = { 0x4E71, 0x4E71, 0x4E71 };
    relevo_cpu *c = setup(code, 3);
    relevo_regs *r = relevo_get_regs(c);
    put32((24 + 3) * 4, HANDLER);
    relevo_set_sr(c, 0x2300);               /* level 3 masked */
    relevo_set_ipl(c, 3);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, CODE + 2);
    relevo_set_sr(c, 0x2200);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER + 2);
    CHECK_EQ(r->sr, 0x2300);                /* the mask is the level */
    CHECK_EQ(get16(SSP - 6), 0x2200);
    CHECK_EQ(get32(SSP - 4), CODE + 2);
    /* Still level 3, now masked: no second interrupt. */
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->a[7], SSP - 6);
    relevo_destroy(c);
}

/* Level 7 cannot be masked, and is taken once per rising edge. */
void test_interrupt_nmi_edge(void)
{
    static const uint16_t code[] = { 0x4E71, 0x4E71, 0x4E71, 0x4E71 };
    relevo_cpu *c = setup(code, 4);
    relevo_regs *r = relevo_get_regs(c);
    relevo_set_sr(c, 0x2700);
    relevo_set_ipl(c, 7);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER + 2);
    CHECK_EQ(r->a[7], SSP - 6);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->a[7], SSP - 6);             /* level held: not again */
    relevo_set_ipl(c, 0);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    relevo_set_ipl(c, 7);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->a[7], SSP - 12);            /* a new edge */
    relevo_destroy(c);
}

void test_stop_until_interrupt(void)
{
    static const uint16_t code[] = { 0x4E72, 0x2000, 0x4E71 };  /* STOP */
    relevo_cpu *c = setup(code, 3);
    relevo_regs *r = relevo_get_regs(c);
    relevo_set_sr(c, 0x2700);
    put32((24 + 2) * 4, HANDLER);
    CHECK_EQ(relevo_run(c, 10), RELEVO_STOPPED);
    CHECK_EQ(r->sr, 0x2000);
    CHECK_EQ(r->pc, CODE + 4);
    CHECK_EQ(relevo_run(c, 10), RELEVO_STOPPED);
    CHECK_EQ(relevo_instructions(c), 1);
    relevo_set_ipl(c, 2);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER + 2);
    CHECK_EQ(get32(SSP - 4), CODE + 4);     /* returns after the STOP */
    relevo_destroy(c);
}

void test_privilege_violation(void)
{
    static const uint16_t code[] = { 0x46FC, 0x2700 };  /* MOVE #,SR */
    relevo_cpu *c = setup(code, 2);
    relevo_regs *r = relevo_get_regs(c);
    r->isp = SSP;
    relevo_set_sr(c, 0x0000);
    r->a[7] = 0x2000;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(r->sr, 0x2000);
    CHECK_EQ(r->usp, 0x2000);
    CHECK_EQ(r->a[7], SSP - 6);
    CHECK_EQ(get32(SSP - 4), CODE);         /* the instruction itself */
    relevo_destroy(c);
}

void test_reset_instruction(void)
{
    static const uint16_t code[] = { 0x4E70 };      /* RESET */
    relevo_cpu *c = setup(code, 1);
    resets = 0;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(resets, 1);
    CHECK_EQ(relevo_get_regs(c)->pc, CODE + 2);
    relevo_destroy(c);
}

void test_trap_and_rte(void)
{
    static const uint16_t code[] = { 0x4E43, 0x4E71 };  /* TRAP #3 */
    relevo_cpu *c = setup(code, 2);
    relevo_regs *r = relevo_get_regs(c);
    put32((32 + 3) * 4, HANDLER);
    put16(HANDLER, 0x4E73);                 /* RTE */
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(get32(SSP - 4), CODE + 2);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, CODE + 2);
    CHECK_EQ(r->a[7], SSP);
    relevo_destroy(c);
}

/* 68000 cycles, from the manual's tables. */
void test_cycles(void)
{
    static const uint16_t code[] = {
        0x2200,             /* MOVE.L D0,D1        4 */
        0x3290,             /* MOVE.W (A0),(A1)   12 */
        0x6002,             /* BRA.S +2           10 */
        0x4E71,             /* (skipped) */
        0x4E43,             /* TRAP #3            34 */
    };
    relevo_cpu *c = setup(code, sizeof code / sizeof code[0]);
    relevo_regs *r = relevo_get_regs(c);
    r->a[0] = 0x2000;
    r->a[1] = 0x2002;
    put32((32 + 3) * 4, HANDLER);
    uint64_t t = relevo_cycles(c);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 4);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 16);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 26);
    CHECK_EQ(r->pc, CODE + 8);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 60);
    CHECK_EQ(r->pc, HANDLER);
    relevo_destroy(c);
}

/* Division by zero (SingleStepTests has no such case): vector 5 with the
 * address of the next instruction, C cleared, the register kept, 38 cycles
 * plus the operand's. */
void test_divide_by_zero(void)
{
    static const uint16_t code[] = {
        0x80C1,             /* DIVU D1,D0         38 */
        0x81FC, 0x0000,     /* DIVS #0,D0         42 */
    };
    relevo_cpu *c = setup(code, sizeof code / sizeof code[0]);
    relevo_regs *r = relevo_get_regs(c);
    put32(5 * 4, HANDLER);
    put16(HANDLER, 0x4E73);                 /* RTE */
    r->d[0] = 0x12345678;
    r->d[1] = 0xFFFF0000;
    r->sr |= RELEVO_SR_C;
    uint64_t t = relevo_cycles(c);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 38);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(get32(SSP - 4), CODE + 2);
    CHECK_EQ(get16(SSP - 6) & RELEVO_SR_C, 0);
    CHECK_EQ(r->d[0], 0x12345678);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);  /* RTE */
    t = relevo_cycles(c);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_cycles(c) - t, 42);
    CHECK_EQ(r->pc, HANDLER);
    CHECK_EQ(get32(SSP - 4), CODE + 6);
    CHECK_EQ(r->d[0], 0x12345678);
    relevo_destroy(c);
}
