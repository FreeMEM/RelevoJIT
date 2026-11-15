/* SPDX-License-Identifier: MPL-2.0
 *
 * Tests of what the 68010 added (task R3a) that the bench against the
 * oracle (tests/oraculo, tools/oraculo) cannot reach: the 68010 itself,
 * bus errors (an Amiga has none to give), interrupts on the master stack,
 * and the frames of those. Expected values from MC68010, MC68020UM and
 * MC68040UM.
 */
#include <string.h>

#include "hooks.h"
#include "check.h"
#include "tests.h"

#define MEM_SIZE 0x40000u   /* 0x40000 and up are unmapped */
#define CODE 0x1000u
#define SSP 0x30000u
#define MSP 0x20000u
#define HANDLER(v) (0x800u + 4u * (v))   /* each vector has its own */

static uint8_t mem[MEM_SIZE];
static relevo_cpu *cpu;
static relevo_regs *r;

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

/* A CPU of `model` in supervisor mode at CODE; every vector has its own
 * handler of NOPs. */
static void start(relevo_model model, const uint16_t *code, size_t n)
{
    memset(mem, 0, sizeof mem);
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.memory = mem;
    cfg.memory_size = sizeof mem;
    cpu = relevo_create(&cfg);
    put32(0, SSP);
    put32(4, CODE);
    for (unsigned v = 2; v < 64; v++)
        put32(v * 4, HANDLER(v));
    for (uint32_t a = HANDLER(0); a < HANDLER(64); a += 2)
        put16(a, 0x4E71);
    for (size_t i = 0; i < n; i++)
        put16(CODE + 2 * (uint32_t)i, code[i]);
    relevo_reset(cpu);
    r = relevo_get_regs(cpu);
}

#define START(model, ...)                                               \
    do {                                                                \
        static const uint16_t code_[] = { __VA_ARGS__ };                \
        start(model, code_, sizeof code_ / sizeof code_[0]);            \
    } while (0)

static void step(void)
{
    relevo_run(cpu, 1);
}

static void done(void)
{
    relevo_destroy(cpu);
    cpu = NULL;
}

void test_movec_68010(void)
{
    /* MOVEC D1,VBR / MOVEC VBR,D2 / TRAP #0 through the new table */
    START(RELEVO_68010, 0x4E7B, 0x1801, 0x4E7A, 0x2801, 0x4E40);
    r->d[1] = 0x2000;
    put32(0x2000 + 32 * 4, 0x3000);
    step();
    step();
    CHECK_EQ(r->vbr, 0x2000);
    CHECK_EQ(r->d[2], 0x2000);
    step();
    CHECK_EQ(r->pc, 0x3000);
    CHECK_EQ(get16(r->a[7] + 6), 32 * 4);       /* format 0 */
    done();

    /* SFC keeps 3 bits; CACR does not exist on the 68010 */
    START(RELEVO_68010, 0x4E7B, 0x1000, 0x4E7A, 0x2000, 0x4E7A, 0x3002);
    r->d[1] = 0xFFFFFFFF;
    step();
    step();
    CHECK_EQ(r->d[2], 7);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_ILLEGAL));
    CHECK_EQ(get32(r->a[7] + 2), CODE + 8);
    done();

    /* MOVEC to USP from supervisor mode; in user mode, a privilege
     * violation */
    START(RELEVO_68020, 0x4E7B, 0x1800, 0x4E7A, 0x1800);
    r->d[1] = 0x12345678;
    step();
    CHECK_EQ(r->usp, 0x12345678);
    relevo_set_sr(cpu, 0x0000);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_PRIVILEGE));
    CHECK_EQ(get32(r->a[7] + 2), CODE + 4);
    done();
}

void test_move_from_sr_privileged(void)
{
    /* MOVE SR,D0 in user mode: allowed on the 68000, privileged later */
    START(RELEVO_68000, 0x40C0);
    relevo_set_sr(cpu, 0x0015);
    step();
    CHECK_EQ(r->d[0] & 0xFFFF, 0x0015);
    done();

    START(RELEVO_68010, 0x40C0);
    relevo_set_sr(cpu, 0x0015);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_PRIVILEGE));
    CHECK_EQ(r->d[0], 0);
    done();

    /* MOVE CCR,D0: 68010 on, user mode too; illegal on the 68000 */
    START(RELEVO_68010, 0x42C0);
    relevo_set_sr(cpu, 0x0015);
    r->d[0] = 0xFFFFFFFF;
    step();
    CHECK_EQ(r->d[0], 0xFFFF0015);
    done();

    START(RELEVO_68000, 0x42C0);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_ILLEGAL));
    done();
}

void test_rtd(void)
{
    /* RTD #8 */
    START(RELEVO_68010, 0x4E74, 0x0008);
    r->a[7] = 0x10000;
    put32(0x10000, 0x2468);
    step();
    CHECK_EQ(r->pc, 0x2468);
    CHECK_EQ(r->a[7], 0x10000 + 4 + 8);
    done();
}

/* A MOVE.W (A0),D0 from an unmapped page: the bus error frame of each
 * model, and RTE back to the instruction. */
static void bus_error_frame(relevo_model model, unsigned format,
                            unsigned size)
{
    START(model, 0x3010);
    r->a[0] = 0x00500002;
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_BUS_ERROR));
    uint32_t sp = r->a[7];
    CHECK_EQ(sp, SSP - size);
    CHECK_EQ(get16(sp), 0x2700);
    CHECK_EQ(get32(sp + 2), CODE);
    CHECK_EQ(get16(sp + 6), format << 12 | VEC_BUS_ERROR * 4);
    switch (format) {
    case 0x8:   /* SSW: DF, RW, FC 5; fault address */
        CHECK_EQ(get16(sp + 8), 0x1000 | 0x0100 | 5);
        CHECK_EQ(get32(sp + 10), 0x00500002);
        break;
    case 0xB:   /* SSW: DF, RW, word, FC 5; data cycle fault address */
        CHECK_EQ(get16(sp + 10), 0x0100 | 0x0040 | 0x0020 | 5);
        CHECK_EQ(get32(sp + 16), 0x00500002);
        break;
    case 0x7:   /* EA; SSW: RW, word, TM 5; fault address */
        CHECK_EQ(get32(sp + 8), 0x00500002);
        CHECK_EQ(get16(sp + 12), 0x0100 | 0x0040 | 5);
        CHECK_EQ(get32(sp + 20), 0x00500002);
        break;
    default:
        break;
    }
    /* the handler maps the page and returns: the MOVE runs again */
    put16(HANDLER(VEC_BUS_ERROR), 0x4E73);
    relevo_map(cpu, 0, MEM_SIZE, RELEVO_PAGE_RAM);
    r->a[0] = 0x100;
    put16(0x100, 0xBEEF);
    step();
    CHECK_EQ(r->pc, CODE);
    CHECK_EQ(r->a[7], SSP);
    step();
    CHECK_EQ(r->d[0] & 0xFFFF, 0xBEEF);
    done();
}

void test_bus_error_frames(void)
{
    bus_error_frame(RELEVO_68010, 0x8, 58);
    bus_error_frame(RELEVO_68020, 0xB, 92);
    bus_error_frame(RELEVO_68030, 0xB, 92);
    bus_error_frame(RELEVO_68040, 0x7, 60);
}

void test_frames_format2(void)
{
    /* TRAPV with V set: format 2 on the 68020 (the TRAPV's address) and
     * on the 68040 (the next instruction's), format 0 on the 68010 */
    static const relevo_model models[] = {
        RELEVO_68010, RELEVO_68020, RELEVO_68040
    };
    for (unsigned i = 0; i < 3; i++) {
        START(models[i], 0x4E76);
        relevo_set_sr(cpu, 0x2702);
        step();
        CHECK_EQ(r->pc, HANDLER(VEC_TRAPV));
        CHECK_EQ(get32(r->a[7] + 2), CODE + 2);
        if (models[i] == RELEVO_68010) {
            CHECK_EQ(r->a[7], SSP - 8);
            CHECK_EQ(get16(r->a[7] + 6), VEC_TRAPV * 4);
        } else {
            CHECK_EQ(r->a[7], SSP - 12);
            CHECK_EQ(get16(r->a[7] + 6), 0x2000 | VEC_TRAPV * 4);
            CHECK_EQ(get32(r->a[7] + 8),
                     models[i] == RELEVO_68020 ? CODE : CODE + 2);
        }
        done();
    }
}

void test_interrupt_master_stack(void)
{
    /* 68020 with M set: format 0 on the master stack, format 1 on the
     * interrupt stack, M cleared; RTE through both */
    START(RELEVO_68020, 0x4E71, 0x4E71);
    relevo_set_sr(cpu, 0x3000);         /* S, M, IPL 0 */
    r->a[7] = MSP;
    relevo_set_ipl(cpu, 3);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_AUTOVECTOR + 3) + 2);   /* one NOP run */
    CHECK_EQ(r->sr & RELEVO_SR_M, 0);
    CHECK_EQ(r->a[7], SSP - 8);
    CHECK_EQ(get16(SSP - 2), 0x1000 | (VEC_AUTOVECTOR + 3) * 4);
    CHECK_EQ(get16(MSP - 8), 0x3000);
    CHECK_EQ(get32(MSP - 6), CODE);
    CHECK_EQ(get16(MSP - 2), (VEC_AUTOVECTOR + 3) * 4);
    relevo_set_ipl(cpu, 0);
    put16(HANDLER(VEC_AUTOVECTOR + 3) + 2, 0x4E73);
    step();
    CHECK_EQ(r->pc, CODE);
    CHECK_EQ(r->sr, 0x3000);
    CHECK_EQ(r->a[7], MSP);
    CHECK_EQ(r->isp, SSP);
    done();
}

void test_format_error(void)
{
    /* RTE with format $8 on a 68020: format error, the RTE's address,
     * stacked where the bad frame's first four words were */
    START(RELEVO_68020, 0x4E73);
    r->a[7] = SSP - 16;
    put16(SSP - 16, 0x2700);
    put32(SSP - 14, 0x4000);
    put16(SSP - 10, 0x8000);
    step();
    CHECK_EQ(r->pc, HANDLER(VEC_FORMAT_ERROR));
    CHECK_EQ(r->a[7], SSP - 16);
    CHECK_EQ(get32(r->a[7] + 2), CODE);
    CHECK_EQ(get16(r->a[7] + 6), VEC_FORMAT_ERROR * 4);
    done();

    /* the same frame is right on a 68010: 29 words */
    START(RELEVO_68010, 0x4E73);
    r->a[7] = SSP - 58;
    put16(SSP - 58, 0x2704);
    put32(SSP - 56, 0x4000);
    put16(SSP - 52, 0x8008);
    step();
    CHECK_EQ(r->pc, 0x4000);
    CHECK_EQ(r->a[7], SSP);
    CHECK_EQ(r->sr, 0x2704);
    done();
}
