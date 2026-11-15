/* SPDX-License-Identifier: MPL-2.0
 *
 * Tests of what the 68020 to 68040 add (task R3b): addressing modes, long
 * multiply and divide, CHK.L, CHK2/CMP2, CAS/CAS2, TRAPcc, PACK/UNPK, bit
 * fields, LINK.L, Bcc.L, CMPI/TST with new operands, MOVE16, CINV/CPUSH
 * and CACR. The expected values come from the descriptions in M68000PRM,
 * MC68020UM and MC68040UM; the opcodes were checked with an assembler.
 *
 * The exception tests look at the vector taken, the PC stacked and the
 * vector offset in the format word, not at the frame format.
 */
#include <string.h>

#include "hooks.h"
#include "check.h"
#include "tests.h"

#define CCR_C RELEVO_SR_C
#define CCR_V RELEVO_SR_V
#define CCR_Z RELEVO_SR_Z
#define CCR_N RELEVO_SR_N
#define CCR_X RELEVO_SR_X

#define MEM_SIZE 0x40000u
#define CODE 0x1000u
#define SSP 0x30000u
#define HANDLER(v) (0x800u + 4u * (v))   /* each vector has its own */

static uint8_t mem[MEM_SIZE];
static relevo_cpu *cpu;
static relevo_regs *r;
static relevo_model model_now;

static void put8(uint32_t a, uint8_t v)
{
    mem[a] = v;
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

/* A CPU of `model` in supervisor mode at CODE, running `code`. */
static void start(relevo_model model, const uint16_t *code, size_t n)
{
    memset(mem, 0, sizeof mem);
    relevo_config cfg = {0};
    cfg.model = model;
    model_now = model;
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
    CHECK_EQ(relevo_run(cpu, 1), RELEVO_BUDGET);
}

static void done(void)
{
    relevo_destroy(cpu);
}

static unsigned ccr(void)
{
    return r->sr & 0x1F;
}

static void set_ccr(unsigned f)
{
    r->sr = (uint16_t)((r->sr & 0xFF00) | f);
}

/* The CPU took `vector` and stacked `pc` (and, after the 68000, the
 * vector offset). */
static void check_exception(unsigned vector, uint32_t pc)
{
    CHECK_EQ(r->pc, HANDLER(vector));
    CHECK_EQ(get32(r->a[7] + 2), pc);
    if (model_now != RELEVO_68000)
        CHECK_EQ(get16(r->a[7] + 6) & 0x0FFF, vector * 4);
}

/* ---- Addressing modes ------------------------------------------------- */

void test_ea_020_brief_scaled(void)
{
    /* LEA (8,A0,D1.L*4),A2 */
    START(RELEVO_68020, 0x45F0, 0x1C08);
    r->a[0] = 0x2000;
    r->d[1] = 3;
    step();
    CHECK_EQ(r->a[2], 0x2014);
    CHECK_EQ(r->pc, CODE + 4);
    done();

    /* The 68000 and 68010 ignore the scale. */
    START(RELEVO_68000, 0x45F0, 0x1C08);
    r->a[0] = 0x2000;
    r->d[1] = 3;
    step();
    CHECK_EQ(r->a[2], 0x200B);
    done();
    START(RELEVO_68010, 0x45F0, 0x1C08);
    r->a[0] = 0x2000;
    r->d[1] = 3;
    step();
    CHECK_EQ(r->a[2], 0x200B);
    done();
}

void test_ea_020_full_no_indirect(void)
{
    /* LEA ($12345678,A0,D1.W*2),A2: long base displacement */
    START(RELEVO_68020, 0x45F0, 0x1330, 0x1234, 0x5678);
    r->a[0] = 0x2000;
    r->d[1] = 0xFFFF0003;
    step();
    CHECK_EQ(r->a[2], 0x1234767E);
    CHECK_EQ(r->pc, CODE + 8);
    done();

    /* LEA ($100.W,ZA0,D1.L*8),A2: base suppressed */
    START(RELEVO_68020, 0x45F0, 0x1FA0, 0x0100);
    r->a[0] = 0x2000;
    r->d[1] = 2;
    step();
    CHECK_EQ(r->a[2], 0x110);
    done();

    /* LEA ($10.W,A0,ZD1),A2: index suppressed */
    START(RELEVO_68020, 0x45F0, 0x1160, 0x0010);
    r->a[0] = 0x2000;
    r->d[1] = 5;
    step();
    CHECK_EQ(r->a[2], 0x2010);
    done();
}

void test_ea_020_memory_indirect(void)
{
    /* MOVE.L ([$10,A0,D1.L*4],8),D0: pre-indexed */
    START(RELEVO_68020, 0x2030, 0x1D22, 0x0010, 0x0008);
    r->a[0] = 0x2000;
    r->d[1] = 1;
    put32(0x2014, 0x3000);
    put32(0x3008, 0xCAFEBABE);
    step();
    CHECK_EQ(r->d[0], 0xCAFEBABE);
    CHECK_EQ(r->pc, CODE + 8);
    done();

    /* MOVE.L ([$10,A0],D1.L*4,8),D0: post-indexed */
    START(RELEVO_68020, 0x2030, 0x1D26, 0x0010, 0x0008);
    r->a[0] = 0x2000;
    r->d[1] = 1;
    put32(0x2010, 0x3100);
    put32(0x310C, 0x11223344);
    step();
    CHECK_EQ(r->d[0], 0x11223344);
    done();

    /* MOVE.L ([A0]),D0 and MOVE.L D0,([A0]): no displacements */
    START(RELEVO_68020, 0x2030, 0x0151, 0x2180, 0x0151);
    r->a[0] = 0x2000;
    put32(0x2000, 0x3200);
    put32(0x3200, 0x55667788);
    step();
    CHECK_EQ(r->d[0], 0x55667788);
    r->d[0] = 0x01020304;
    step();
    CHECK_EQ(get32(0x3200), 0x01020304);
    done();

    /* LEA ([-2.W,A0],-4.W),A2: word displacements are sign-extended */
    START(RELEVO_68020, 0x45F0, 0x0162, 0xFFFE, 0xFFFC);
    r->a[0] = 0x2002;
    put32(0x2000, 0x3400);
    step();
    CHECK_EQ(r->a[2], 0x33FC);
    CHECK_EQ(r->pc, CODE + 8);
    done();
}

void test_ea_020_pc_relative(void)
{
    /* LEA ($100.W,PC,D1.W*2),A2: base is the extension word's address */
    START(RELEVO_68020, 0x45FB, 0x1320, 0x0100);
    r->d[1] = 4;
    step();
    CHECK_EQ(r->a[2], CODE + 2 + 0x100 + 8);
    done();

    /* LEA ($3000.L,ZPC),A2 */
    START(RELEVO_68020, 0x45FB, 0x01F0, 0x0000, 0x3000);
    step();
    CHECK_EQ(r->a[2], 0x3000);
    done();

    /* MOVE.L ([$20.W,PC]),D0 */
    START(RELEVO_68020, 0x203B, 0x0161, 0x0020);
    put32(CODE + 2 + 0x20, 0x3300);
    put32(0x3300, 0x9ABCDEF0);
    step();
    CHECK_EQ(r->d[0], 0x9ABCDEF0);
    done();
}

void test_ea_020_reserved(void)
{
    /* Base displacement size 00: illegal instruction. */
    START(RELEVO_68020, 0x45F0, 0x0100);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
    /* I/IS 100: illegal. */
    START(RELEVO_68020, 0x45F0, 0x0114);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
    /* Index suppressed with I/IS 101: illegal. */
    START(RELEVO_68020, 0x45F0, 0x0155);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
    /* The 68000 reads the same words as a brief extension. */
    START(RELEVO_68000, 0x45F0, 0x0100);
    r->a[0] = 0x2000;
    r->d[0] = 0x10;
    step();
    CHECK_EQ(r->a[2], 0x2010);
    done();
}

/* ---- MULx.L, DIVx.L --------------------------------------------------- */

void test_mul_l(void)
{
    /* MULU.L D1,D0: 32 bits, overflow; X is kept */
    START(RELEVO_68020, 0x4C01, 0x0000);
    set_ccr(CCR_X);
    r->d[0] = 0x10000;
    r->d[1] = 0x10000;
    step();
    CHECK_EQ(r->d[0], 0);
    /* N and Z come from the 64-bit product (as FS-UAE, tests/oraculo) */
    CHECK_EQ(ccr(), CCR_X | CCR_V);
    done();

    /* MULS.L D1,D0 */
    START(RELEVO_68020, 0x4C01, 0x0800);
    r->d[0] = (uint32_t)-2;
    r->d[1] = 3;
    step();
    CHECK_EQ(r->d[0], (uint32_t)-6);
    CHECK_EQ(ccr(), CCR_N);
    done();

    START(RELEVO_68020, 0x4C01, 0x0800);
    r->d[0] = 0x40000000;
    r->d[1] = 2;
    step();
    CHECK_EQ(r->d[0], 0x80000000);
    CHECK_EQ(ccr(), CCR_V);
    done();

    START(RELEVO_68020, 0x4C01, 0x0800);
    r->d[0] = 0xFFFFFFFF;
    r->d[1] = 0xFFFFFFFF;
    set_ccr(CCR_C | CCR_V);
    step();
    CHECK_EQ(r->d[0], 1);
    CHECK_EQ(ccr(), 0);
    done();

    /* MULU.L D1,D2:D0: 64 bits */
    START(RELEVO_68020, 0x4C01, 0x0402);
    r->d[0] = 0xFFFFFFFF;
    r->d[1] = 0xFFFFFFFF;
    step();
    CHECK_EQ(r->d[2], 0xFFFFFFFE);
    CHECK_EQ(r->d[0], 1);
    CHECK_EQ(ccr(), CCR_N);
    done();

    /* MULS.L D1,D2:D0 */
    START(RELEVO_68020, 0x4C01, 0x0C02);
    r->d[0] = 0xFFFFFFFF;
    r->d[1] = 1;
    step();
    CHECK_EQ(r->d[2], 0xFFFFFFFF);
    CHECK_EQ(r->d[0], 0xFFFFFFFF);
    CHECK_EQ(ccr(), CCR_N);
    done();

    START(RELEVO_68020, 0x4C01, 0x0C02);
    r->d[0] = 0;
    r->d[1] = (uint32_t)-5;
    r->d[2] = 0x1234;
    step();
    CHECK_EQ(r->d[2], 0);
    CHECK_EQ(r->d[0], 0);
    CHECK_EQ(ccr(), CCR_Z);
    done();

    /* MULU.L #3,D0: the immediate follows the extension word */
    START(RELEVO_68020, 0x4C3C, 0x0000, 0x0000, 0x0003);
    r->d[0] = 7;
    step();
    CHECK_EQ(r->d[0], 21);
    CHECK_EQ(r->pc, CODE + 8);
    done();
}

void test_div_l(void)
{
    /* DIVU.L D1,D0 */
    START(RELEVO_68020, 0x4C41, 0x0000);
    r->d[0] = 100;
    r->d[1] = 7;
    set_ccr(CCR_X | CCR_C | CCR_V);
    step();
    CHECK_EQ(r->d[0], 14);
    CHECK_EQ(ccr(), CCR_X);
    done();

    /* DIVUL.L D1,D2:D0 */
    START(RELEVO_68020, 0x4C41, 0x0002);
    r->d[0] = 100;
    r->d[1] = 7;
    step();
    CHECK_EQ(r->d[0], 14);
    CHECK_EQ(r->d[2], 2);
    done();

    /* DIVU.L D1,D2:D0: 64 bits */
    START(RELEVO_68020, 0x4C41, 0x0402);
    r->d[2] = 1;
    r->d[0] = 0;
    r->d[1] = 2;
    step();
    CHECK_EQ(r->d[0], 0x80000000);
    CHECK_EQ(r->d[2], 0);
    CHECK_EQ(ccr(), CCR_N);
    done();

    /* ... overflow: registers unchanged, V set */
    START(RELEVO_68020, 0x4C41, 0x0402);
    r->d[2] = 2;
    r->d[0] = 0;
    r->d[1] = 2;
    step();
    CHECK_EQ(r->d[0], 0);
    CHECK_EQ(r->d[2], 2);
    CHECK(ccr() & CCR_V);
    CHECK(!(ccr() & CCR_C));
    done();

    /* DIVS.L D1,D0 and DIVSL.L D1,D2:D0: remainder has the dividend's
     * sign */
    START(RELEVO_68020, 0x4C41, 0x0800);
    r->d[0] = (uint32_t)-100;
    r->d[1] = 7;
    step();
    CHECK_EQ(r->d[0], (uint32_t)-14);
    CHECK_EQ(ccr(), CCR_N);
    done();
    START(RELEVO_68020, 0x4C41, 0x0802);
    r->d[0] = (uint32_t)-100;
    r->d[1] = 7;
    step();
    CHECK_EQ(r->d[0], (uint32_t)-14);
    CHECK_EQ(r->d[2], (uint32_t)-2);
    done();

    /* DIVS.L: $80000000 / -1 overflows */
    START(RELEVO_68020, 0x4C41, 0x0800);
    r->d[0] = 0x80000000;
    r->d[1] = 0xFFFFFFFF;
    step();
    CHECK_EQ(r->d[0], 0x80000000);
    CHECK(ccr() & CCR_V);
    done();

    /* DIVS.L D1,D2:D0: -2^32 / 2 fits, -2^32 / -2 does not */
    START(RELEVO_68020, 0x4C41, 0x0C02);
    r->d[2] = 0xFFFFFFFF;
    r->d[0] = 0;
    r->d[1] = 2;
    step();
    CHECK_EQ(r->d[0], 0x80000000);
    CHECK_EQ(r->d[2], 0);
    CHECK_EQ(ccr(), CCR_N);
    done();
    START(RELEVO_68020, 0x4C41, 0x0C02);
    r->d[2] = 0xFFFFFFFF;
    r->d[0] = 0;
    r->d[1] = (uint32_t)-2;
    step();
    CHECK_EQ(r->d[0], 0);
    CHECK_EQ(r->d[2], 0xFFFFFFFF);
    CHECK(ccr() & CCR_V);
    done();
    START(RELEVO_68020, 0x4C41, 0x0C02);
    r->d[2] = 0;
    r->d[0] = 100;
    r->d[1] = (uint32_t)-7;
    step();
    CHECK_EQ(r->d[0], (uint32_t)-14);
    CHECK_EQ(r->d[2], 2);
    done();

    /* Division by zero: vector 5, the next instruction's address,
     * registers and flags unchanged (as FS-UAE, tests/oraculo) */
    START(RELEVO_68020, 0x4C41, 0x0402);
    r->d[0] = 100;
    r->d[1] = 0;
    r->d[2] = 5;
    set_ccr(CCR_C);
    step();
    check_exception(VEC_ZERO_DIVIDE, CODE + 4);
    CHECK_EQ(r->d[0], 100);
    CHECK_EQ(r->d[2], 5);
    CHECK(get16(r->a[7]) & CCR_C);
    done();
}

/* ---- EXTB.L, CHK.L, CHK2, CMP2 ---------------------------------------- */

void test_extb_l(void)
{
    START(RELEVO_68020, 0x49C0, 0x49C0);
    r->d[0] = 0x12345680;
    step();
    CHECK_EQ(r->d[0], 0xFFFFFF80);
    CHECK_EQ(ccr(), CCR_N);
    r->d[0] = 0x1234567F;
    step();
    CHECK_EQ(r->d[0], 0x7F);
    CHECK_EQ(ccr(), 0);
    done();

    /* Not on the 68000 */
    START(RELEVO_68000, 0x49C0);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
}

void test_chk_l(void)
{
    /* CHK.L D1,D0 */
    START(RELEVO_68020, 0x4101, 0x4101);
    r->d[1] = 10;
    r->d[0] = 5;
    step();
    CHECK_EQ(r->pc, CODE + 2);
    r->d[0] = 0;
    set_ccr(CCR_V | CCR_C);
    step();
    CHECK_EQ(r->pc, CODE + 4);
    /* in range: the flags stay (as FS-UAE, tests/oraculo) */
    CHECK_EQ(ccr(), CCR_V | CCR_C);
    done();

    START(RELEVO_68020, 0x4101);
    r->d[1] = 10;
    r->d[0] = 0xFFFFFFFF;
    step();
    check_exception(VEC_CHK, CODE + 2);
    CHECK(get16(r->a[7]) & CCR_N);
    done();

    /* CHK.L #10,D0: above the bound, N clear */
    START(RELEVO_68020, 0x413C, 0x0000, 0x000A);
    r->d[0] = 0x10;
    set_ccr(CCR_N);
    step();
    check_exception(VEC_CHK, CODE + 6);
    CHECK(!(get16(r->a[7]) & CCR_N));
    done();
}

/* CMP2/CHK2 with `bounds` (two values of `size` bytes at 0x2000) and the
 * register value; returns the CCR. */
static unsigned cmp2(uint16_t op, uint16_t ext, unsigned size, uint32_t lo,
                     uint32_t hi, unsigned rn, uint32_t v, unsigned ccr_in)
{
    uint16_t code[2] = { op, ext };
    start(RELEVO_68020, code, 2);
    r->a[0] = 0x2000;
    if (size == 1) {
        put8(0x2000, (uint8_t)lo);
        put8(0x2001, (uint8_t)hi);
    } else if (size == 2) {
        put16(0x2000, (uint16_t)lo);
        put16(0x2002, (uint16_t)hi);
    } else {
        put32(0x2000, lo);
        put32(0x2004, hi);
    }
    if (rn < 8)
        r->d[rn] = v;
    else
        r->a[rn - 8] = v;
    set_ccr(ccr_in);
    step();
    unsigned f = ccr();
    done();
    return f;
}

void test_cmp2(void)
{
    /* CMP2.B (A0),D0: only the low byte of D0 */
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0x15, 0), 0);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0x10, 0), CCR_Z);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0x20, 0), CCR_Z);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0x25, 0), CCR_C);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0x05, 0), CCR_C);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0x10, 0x20, 0, 0xFFFFFF15, 0), 0);
    /* signed bounds -5..5 */
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0xFB, 0x05, 0, 0xFE, 0), 0);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0xFB, 0x05, 0, 0x06, 0), CCR_C);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0xFB, 0x05, 0, 0x80, 0), CCR_C);
    CHECK_EQ(cmp2(0x00D0, 0x0000, 1, 0xFB, 0x05, 0, 0xFB, 0), CCR_Z);
    /* CMP2.W (A0),D0, unsigned bounds $1000..$9000 */
    CHECK_EQ(cmp2(0x02D0, 0x0000, 2, 0x1000, 0x9000, 0, 0x8000, 0), 0);
    CHECK_EQ(cmp2(0x02D0, 0x0000, 2, 0x1000, 0x9000, 0, 0xA000, 0), CCR_C);
    /* CMP2.W (A0),A1: bounds sign-extended, the whole An compared */
    CHECK_EQ(cmp2(0x02D0, 0x9000, 2, 0x1000, 0x9000, 9, 0x8000, 0), 0);
    CHECK_EQ(cmp2(0x02D0, 0x9000, 2, 0x1000, 0x9000, 9, 0xFFFFA000, 0),
             CCR_C);
    CHECK_EQ(cmp2(0x02D0, 0x9000, 2, 0x1000, 0x9000, 9, 0xFFFF9000, 0),
             CCR_Z);
    /* CMP2.L: the whole signed range; N, V and X are kept */
    CHECK_EQ(cmp2(0x04D0, 0x0000, 4, 0x80000000, 0x7FFFFFFF, 0, 0,
                  CCR_X | CCR_N | CCR_V | CCR_C),
             CCR_X | CCR_N | CCR_V);
    CHECK_EQ(cmp2(0x04D0, 0x0000, 4, 100, 200, 0, 201, 0), CCR_C);
}

void test_chk2(void)
{
    /* CHK2.L (A0),D0, bounds 0..100 */
    CHECK_EQ(cmp2(0x04D0, 0x0800, 4, 0, 100, 0, 50, 0), 0);

    static const uint16_t code[] = { 0x04D0, 0x0800 };
    start(RELEVO_68020, code, 2);
    r->a[0] = 0x2000;
    put32(0x2000, 0);
    put32(0x2004, 100);
    r->d[0] = 101;
    step();
    check_exception(VEC_CHK, CODE + 4);
    CHECK(get16(r->a[7]) & CCR_C);
    done();

    /* CHK2.W (d16,PC),D0: bounds -16..16 after the code */
    START(RELEVO_68020, 0x02FA, 0x0800, 0x000C);
    put16(CODE + 0x10, 0xFFF0);
    put16(CODE + 0x12, 0x0010);
    r->d[0] = 0xFFFF;
    step();
    CHECK_EQ(r->pc, CODE + 6);
    CHECK_EQ(ccr(), 0);
    done();
    START(RELEVO_68020, 0x02FA, 0x0800, 0x000C);
    put16(CODE + 0x10, 0xFFF0);
    put16(CODE + 0x12, 0x0010);
    r->d[0] = 0x20;
    step();
    check_exception(VEC_CHK, CODE + 6);
    done();
}

/* ---- CAS, CAS2 -------------------------------------------------------- */

void test_cas(void)
{
    /* CAS.L D1,D2,(A0): equal */
    START(RELEVO_68020, 0x0ED0, 0x0081);
    r->a[0] = 0x2000;
    put32(0x2000, 0x12345678);
    r->d[1] = 0x12345678;
    r->d[2] = 0xAABBCCDD;
    step();
    CHECK_EQ(get32(0x2000), 0xAABBCCDD);
    CHECK_EQ(ccr(), CCR_Z);
    done();

    /* not equal: the operand goes to Dc */
    START(RELEVO_68020, 0x0ED0, 0x0081);
    r->a[0] = 0x2000;
    put32(0x2000, 0x12345678);
    r->d[1] = 0x12345679;
    r->d[2] = 0xAABBCCDD;
    step();
    CHECK_EQ(get32(0x2000), 0x12345678);
    CHECK_EQ(r->d[1], 0x12345678);
    CHECK_EQ(ccr(), CCR_N | CCR_C);
    done();

    /* CAS.B D1,D2,(A0)+ */
    START(RELEVO_68020, 0x0AD8, 0x0081);
    r->a[0] = 0x2000;
    put8(0x2000, 0x42);
    r->d[1] = 0xFFFFFF42;
    r->d[2] = 0x99;
    step();
    CHECK_EQ(mem[0x2000], 0x99);
    CHECK_EQ(r->a[0], 0x2001);
    CHECK_EQ(ccr(), CCR_Z);
    done();

    /* CAS.W D1,D2,-(A0): not equal, only the low word of Dc changes */
    START(RELEVO_68020, 0x0CE0, 0x0081);
    r->a[0] = 0x2002;
    put16(0x2000, 0x1111);
    r->d[1] = 0xABCD2222;
    step();
    CHECK_EQ(r->d[1], 0xABCD1111);
    CHECK_EQ(r->a[0], 0x2000);
    CHECK_EQ(ccr(), CCR_N | CCR_C);
    done();
}

static void cas2_setup(uint32_t d0, uint32_t d1)
{
    r->a[0] = 0x2000;
    r->a[1] = 0x2010;
    put32(0x2000, 1);
    put32(0x2010, 2);
    r->d[0] = d0;
    r->d[1] = d1;
    r->d[2] = 0x10;
    r->d[3] = 0x20;
}

void test_cas2(void)
{
    /* CAS2.L D0:D1,D2:D3,(A0):(A1): both equal */
    START(RELEVO_68020, 0x0EFC, 0x8080, 0x90C1);
    cas2_setup(1, 2);
    step();
    CHECK_EQ(get32(0x2000), 0x10);
    CHECK_EQ(get32(0x2010), 0x20);
    CHECK_EQ(ccr(), CCR_Z);
    CHECK_EQ(r->pc, CODE + 6);
    done();

    /* the first differs: flags from it, both operands loaded */
    START(RELEVO_68020, 0x0EFC, 0x8080, 0x90C1);
    cas2_setup(5, 2);
    step();
    CHECK_EQ(get32(0x2000), 1);
    CHECK_EQ(get32(0x2010), 2);
    CHECK_EQ(r->d[0], 1);
    CHECK_EQ(r->d[1], 2);
    CHECK_EQ(ccr(), CCR_N | CCR_C);
    done();

    /* the second differs: flags from the second */
    START(RELEVO_68020, 0x0EFC, 0x8080, 0x90C1);
    cas2_setup(1, 1);
    step();
    CHECK_EQ(get32(0x2000), 1);
    CHECK_EQ(r->d[0], 1);
    CHECK_EQ(r->d[1], 2);
    CHECK_EQ(ccr(), 0);
    done();

    /* CAS2.L D0:D0,...: Dc1 = Dc2, fails: D0 gets the first operand */
    START(RELEVO_68020, 0x0EFC, 0x8080, 0x90C0);
    cas2_setup(5, 0);
    step();
    CHECK_EQ(r->d[0], 1);
    done();

    /* CAS2.W D0:D1,D2:D3,(D4):(D5): data registers as addresses, words */
    START(RELEVO_68020, 0x0CFC, 0x4080, 0x50C1);
    r->d[4] = 0x2000;
    r->d[5] = 0x2010;
    put16(0x2000, 0xAAAA);
    put16(0x2010, 0xBBBB);
    r->d[0] = 0xFFFFAAAA;
    r->d[1] = 0x0000BBBB;
    r->d[2] = 0x1234;
    r->d[3] = 0x5678;
    step();
    CHECK_EQ(get16(0x2000), 0x1234);
    CHECK_EQ(get16(0x2010), 0x5678);
    CHECK_EQ(ccr(), CCR_Z);
    done();
}

/* ---- TRAPcc ----------------------------------------------------------- */

void test_trapcc(void)
{
    /* TRAPEQ */
    START(RELEVO_68020, 0x57FC);
    set_ccr(CCR_Z);
    step();
    check_exception(VEC_TRAPV, CODE + 2);
    done();
    START(RELEVO_68020, 0x57FC);
    step();
    CHECK_EQ(r->pc, CODE + 2);
    done();

    /* TRAPNE.W #$1234: not taken, the operand skipped */
    START(RELEVO_68020, 0x56FA, 0x1234);
    set_ccr(CCR_Z);
    step();
    CHECK_EQ(r->pc, CODE + 4);
    done();

    /* TRAPT.L #$12345678 */
    START(RELEVO_68020, 0x50FB, 0x1234, 0x5678);
    step();
    check_exception(VEC_TRAPV, CODE + 6);
    done();

    /* TRAPF */
    START(RELEVO_68020, 0x51FC);
    step();
    CHECK_EQ(r->pc, CODE + 2);
    done();
}

/* ---- PACK, UNPK ------------------------------------------------------- */

void test_pack_unpk(void)
{
    /* PACK D0,D1,#0 */
    START(RELEVO_68020, 0x8340, 0x0000);
    r->d[0] = 0x0305;
    r->d[1] = 0xFFFFFFFF;
    set_ccr(0x1F);
    step();
    CHECK_EQ(r->d[1], 0xFFFFFF35);
    CHECK_EQ(ccr(), 0x1F);
    done();

    /* PACK D0,D1,#-$3030: ASCII "12" */
    START(RELEVO_68020, 0x8340, 0xCFD0);
    r->d[0] = 0x3132;
    r->d[1] = 0;
    step();
    CHECK_EQ(r->d[1], 0x12);
    done();

    /* PACK -(A0),-(A1),#-$3030 */
    START(RELEVO_68020, 0x8348, 0xCFD0);
    r->a[0] = 0x2002;
    r->a[1] = 0x2101;
    put8(0x2000, 0x31);
    put8(0x2001, 0x32);
    step();
    CHECK_EQ(mem[0x2100], 0x12);
    CHECK_EQ(r->a[0], 0x2000);
    CHECK_EQ(r->a[1], 0x2100);
    done();

    /* UNPK D0,D1,#$3030 */
    START(RELEVO_68020, 0x8380, 0x3030);
    r->d[0] = 0x12;
    r->d[1] = 0xFFFFFFFF;
    step();
    CHECK_EQ(r->d[1], 0xFFFF3132);
    done();

    /* UNPK -(A0),-(A1),#$3030 */
    START(RELEVO_68020, 0x8388, 0x3030);
    r->a[0] = 0x2001;
    r->a[1] = 0x2102;
    put8(0x2000, 0x47);
    step();
    CHECK_EQ(mem[0x2100], 0x34);
    CHECK_EQ(mem[0x2101], 0x37);
    CHECK_EQ(r->a[0], 0x2000);
    CHECK_EQ(r->a[1], 0x2100);
    done();
}

/* ---- Bit fields ------------------------------------------------------- */

/* Runs one bit field instruction on a 68020 with D0, D1, D2, D3 and A0
 * set; memory at 0x1FF8 holds `bytes`. */
static void bf_run(uint16_t op, uint16_t ext, uint32_t d0, uint32_t d1,
                   uint32_t d2, uint32_t d3, const uint8_t *bytes, size_t n)
{
    uint16_t code[2] = { op, ext };
    start(RELEVO_68020, code, 2);
    for (size_t i = 0; i < n; i++)
        mem[0x1FF8 + i] = bytes[i];
    r->d[0] = d0;
    r->d[1] = d1;
    r->d[2] = d2;
    r->d[3] = d3;
    r->a[0] = 0x2000;
    set_ccr(CCR_X | CCR_V | CCR_C);
    step();
    CHECK_EQ(r->pc, CODE + 4);
}

void test_bf_register(void)
{
    /* BFEXTU D0{4:8},D1 */
    bf_run(0xE9C0, 0x1108, 0x12345678, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 0x23);
    CHECK_EQ(ccr(), CCR_X);
    done();
    /* BFEXTS D0{0:4},D1 */
    bf_run(0xEBC0, 0x1004, 0x92345678, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 0xFFFFFFF9);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();
    /* BFEXTU D0{28:8},D1: wraps from bit 0 to bit 31 */
    bf_run(0xE9C0, 0x1708, 0x12345678, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 0x81);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();
    /* BFEXTU D0{0:0},D1: width 0 is 32 */
    bf_run(0xE9C0, 0x1000, 0x12345678, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 0x12345678);
    done();
    /* BFEXTU D0{D2:D3},D1: both modulo 32 */
    bf_run(0xE9C0, 0x18A3, 0x12345678, 0, 36, 40, NULL, 0);
    CHECK_EQ(r->d[1], 0x23);
    done();
    /* BFINS D1,D0{28:8}: wraps; flags from the value inserted */
    bf_run(0xEFC0, 0x1708, 0x12345678, 0xFFFFFFA5, 0, 0, NULL, 0);
    CHECK_EQ(r->d[0], 0x5234567A);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();
    /* BFCHG D0{0:0} */
    bf_run(0xEAC0, 0x0000, 0x12345678, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[0], 0xEDCBA987);
    CHECK_EQ(ccr(), CCR_X);
    done();
    /* BFFFO D0{8:8},D1 */
    bf_run(0xEDC0, 0x1208, 0x00100000, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 11);
    done();
    bf_run(0xEDC0, 0x1208, 0xFF00FFFF, 0, 0, 0, NULL, 0);
    CHECK_EQ(r->d[1], 16);
    CHECK_EQ(ccr(), CCR_X | CCR_Z);
    done();
    /* BFTST D0{8:8} */
    bf_run(0xE8C0, 0x0208, 0x00800000, 0, 0, 0, NULL, 0);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    CHECK_EQ(r->d[0], 0x00800000);
    done();
}

void test_bf_memory(void)
{
    /* bytes at 0x1FF8..; A0 = 0x2000 is bytes[8] */
    static const uint8_t b1[] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                  0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    /* BFEXTU (A0){4:16},D1: three bytes */
    bf_run(0xE9D0, 0x1110, 0, 0, 0, 0, b1, sizeof b1);
    CHECK_EQ(r->d[1], 0x2345);
    done();
    /* BFEXTU (A0){7:0},D1: 32 bits over five bytes */
    bf_run(0xE9D0, 0x11C0, 0, 0, 0, 0, b1, sizeof b1);
    CHECK_EQ(r->d[1], 0x1A2B3C4D);
    done();

    /* BFEXTU (A0){D2:8},D1 with negative offsets */
    static const uint8_t b2[] = { 0, 0, 0, 0, 0, 0, 0, 0x01, 0x80 };
    bf_run(0xE9D0, 0x1888, 0, 0, (uint32_t)-1, 0, b2, sizeof b2);
    CHECK_EQ(r->d[1], 0xC0);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();
    static const uint8_t b3[] = { 0, 0, 0, 0, 0, 0, 0, 0xAB, 0x12 };
    bf_run(0xE9D0, 0x1888, 0, 0, (uint32_t)-4, 0, b3, sizeof b3);
    CHECK_EQ(r->d[1], 0xB1);
    done();
    bf_run(0xE9D0, 0x1888, 0, 0, (uint32_t)-64, 0, b2, sizeof b2);
    CHECK_EQ(r->d[1], 0);
    CHECK_EQ(ccr(), CCR_X | CCR_Z);
    done();

    /* A large offset from D2: 256 bits on */
    static const uint8_t none[1] = { 0 };
    bf_run(0xE9D0, 0x1888, 0, 0, 256, 0, none, 0);
    CHECK_EQ(r->d[1], 0);
    done();
    {
        static const uint16_t code[] = { 0xE9D0, 0x1888 };
        start(RELEVO_68020, code, 2);
        r->a[0] = 0x2000;
        r->d[2] = 256;
        mem[0x2020] = 0x5A;
        step();
        CHECK_EQ(r->d[1], 0x5A);
        done();
    }

    /* BFINS D1,(A0){4:12}: the bits around stay */
    bf_run(0xEFD0, 0x110C, 0, 0xFFFFFABC, 0, 0, b1, sizeof b1);
    CHECK_EQ(mem[0x2000], 0x1A);
    CHECK_EQ(mem[0x2001], 0xBC);
    CHECK_EQ(mem[0x2002], 0x56);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();

    /* BFCLR (A0){0:8}, BFSET (A0){4:8}: flags from the old field */
    static const uint8_t b4[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x00 };
    bf_run(0xECD0, 0x0008, 0, 0, 0, 0, b4, sizeof b4);
    CHECK_EQ(mem[0x2000], 0);
    CHECK_EQ(ccr(), CCR_X | CCR_N);
    done();
    static const uint8_t b5[] = { 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 0x00,
                                  0xFF };
    bf_run(0xEED0, 0x0108, 0, 0, 0, 0, b5, sizeof b5);
    CHECK_EQ(mem[0x1FFF], 0xFF);
    CHECK_EQ(mem[0x2000], 0x0F);
    CHECK_EQ(mem[0x2001], 0xF0);
    CHECK_EQ(mem[0x2002], 0xFF);
    CHECK_EQ(ccr(), CCR_X | CCR_Z);
    done();

    /* BFFFO (A0){D2:8},D1: the offset in the result is the signed one */
    static const uint8_t b6[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x20 };
    {
        static const uint16_t code[] = { 0xEDD0, 0x1888 };
        start(RELEVO_68020, code, 2);
        for (size_t i = 0; i < sizeof b6; i++)
            mem[0x1FF8 + i] = b6[i];
        r->a[0] = 0x2001;
        r->d[2] = (uint32_t)-4;
        step();
        CHECK_EQ(r->d[1], 2);
        done();
    }

    /* BFTST (d16,PC){0:8} */
    START(RELEVO_68020, 0xE8FA, 0x0008, 0x000C);
    put8(CODE + 0x10, 0x00);
    step();
    CHECK_EQ(ccr(), CCR_Z);
    CHECK_EQ(r->pc, CODE + 6);
    done();

    /* Not on the 68000 */
    START(RELEVO_68000, 0xE9C0, 0x1108);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
}

/* ---- LINK.L, Bcc.L, CMPI, TST ----------------------------------------- */

void test_link_l(void)
{
    /* LINK.L A6,#-$12000 */
    START(RELEVO_68020, 0x480E, 0xFFFE, 0xE000);
    r->a[6] = 0x11111111;
    step();
    CHECK_EQ(get32(SSP - 4), 0x11111111);
    CHECK_EQ(r->a[6], SSP - 4);
    CHECK_EQ(r->a[7], SSP - 4 - 0x12000);
    CHECK_EQ(r->pc, CODE + 6);
    done();
}

void test_branch_long(void)
{
    /* BRA.L +$12340 */
    START(RELEVO_68020, 0x60FF, 0x0001, 0x2340);
    step();
    CHECK_EQ(r->pc, CODE + 2 + 0x12340);
    done();
    /* BEQ.L, not taken and taken */
    START(RELEVO_68020, 0x67FF, 0x0000, 0x00FE);
    step();
    CHECK_EQ(r->pc, CODE + 6);
    done();
    START(RELEVO_68020, 0x67FF, 0x0000, 0x00FE);
    set_ccr(CCR_Z);
    step();
    CHECK_EQ(r->pc, CODE + 2 + 0xFE);
    done();
    /* BSR.L */
    START(RELEVO_68020, 0x61FF, 0x0000, 0x0FFE);
    step();
    CHECK_EQ(r->pc, CODE + 2 + 0xFFE);
    CHECK_EQ(get32(r->a[7]), CODE + 6);
    done();
}

void test_cmpi_tst_020(void)
{
    /* CMPI.W #$1234,(d16,PC) */
    START(RELEVO_68020, 0x0C7A, 0x1234, 0x000C);
    put16(CODE + 0x10, 0x1234);
    step();
    CHECK_EQ(ccr(), CCR_Z);
    CHECK_EQ(r->pc, CODE + 6);
    done();
    /* CMPI.L #$12345678,(8,PC,D0.W) */
    START(RELEVO_68020, 0x0CBB, 0x1234, 0x5678, 0x0008);
    r->d[0] = 4;
    put32(CODE + 6 + 8 + 4, 0x12345679);
    step();
    CHECK_EQ(ccr(), 0);
    done();
    START(RELEVO_68020, 0x0CBB, 0x1234, 0x5679, 0x0008);
    r->d[0] = 4;
    put32(CODE + 6 + 8 + 4, 0x12345678);
    step();
    CHECK_EQ(ccr(), CCR_N | CCR_C);
    done();

    /* TST.L A0, TST.W #0, TST.B (4,PC,D0.W) */
    START(RELEVO_68020, 0x4A88, 0x4A7C, 0x0000, 0x4A3B, 0x0004);
    r->a[0] = 0x80000000;
    r->d[0] = 2;
    put8(CODE + 8 + 4 + 2, 0x80);
    step();
    CHECK_EQ(ccr(), CCR_N);
    step();
    CHECK_EQ(ccr(), CCR_Z);
    step();
    CHECK_EQ(ccr(), CCR_N);
    CHECK_EQ(r->pc, CODE + 10);
    done();

    /* The 68000 does not have them. */
    START(RELEVO_68000, 0x4A88);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
    START(RELEVO_68000, 0x0C7A, 0x1234, 0x000C);
    step();
    check_exception(VEC_ILLEGAL, CODE);
    done();
}

/* ---- 68040: MOVE16, CINV, CPUSH; CACR ---------------------------------- */

void test_move16(void)
{
    /* MOVE16 (A0)+,(A1)+: aligned down to 16 bytes */
    START(RELEVO_68040, 0xF620, 0x9000);
    for (uint32_t i = 0; i < 16; i++)
        mem[0x2000 + i] = (uint8_t)(i + 1);
    r->a[0] = 0x2008;
    r->a[1] = 0x3004;
    step();
    for (uint32_t i = 0; i < 16; i++)
        CHECK_EQ(mem[0x3000 + i], i + 1);
    CHECK_EQ(mem[0x3010], 0);
    CHECK_EQ(r->a[0], 0x2018);
    CHECK_EQ(r->a[1], 0x3014);
    done();

    /* MOVE16 $3000,(A1)+ and MOVE16 (A1),$3000 */
    START(RELEVO_68040, 0xF609, 0x0000, 0x3000, 0xF611, 0x0000, 0x3000);
    put32(0x3000, 0xAABBCCDD);
    put32(0x300C, 0x11223344);
    r->a[1] = 0x2000;
    step();
    CHECK_EQ(get32(0x2000), 0xAABBCCDD);
    CHECK_EQ(get32(0x200C), 0x11223344);
    CHECK_EQ(r->a[1], 0x2010);
    put32(0x2010, 0x55667788);
    step();
    CHECK_EQ(get32(0x3000), 0x55667788);
    CHECK_EQ(r->a[1], 0x2010);
    CHECK_EQ(r->pc, CODE + 12);
    done();

    /* Line F on the 68020 */
    START(RELEVO_68020, 0xF620, 0x9000);
    step();
    check_exception(VEC_LINE_F, CODE);
    done();
}

void test_cinv_cpush(void)
{
    /* CINVA BC, CPUSHL IC,(A0), CINVP IC,(A0), CINVL DC,(A0), CPUSHA BC */
    START(RELEVO_68040, 0xF4D8, 0xF4A8, 0xF490, 0xF448, 0xF4F8);
    r->a[0] = 0x2000;
    CHECK_EQ(relevo_run(cpu, 5), RELEVO_BUDGET);
    CHECK_EQ(r->pc, CODE + 10);
    done();

    /* Privileged */
    START(RELEVO_68040, 0xF4D8);
    relevo_set_sr(cpu, 0x0000);
    step();
    check_exception(VEC_PRIVILEGE, CODE);
    done();

    /* Scope 00 is not an instruction; the 68030 has none of them */
    START(RELEVO_68040, 0xF4C0);
    step();
    check_exception(VEC_LINE_F, CODE);
    done();
    START(RELEVO_68030, 0xF4D8);
    step();
    check_exception(VEC_LINE_F, CODE);
    done();
}

void test_cacr(void)
{
    static const struct {
        relevo_model model;
        uint32_t kept;
    } cases[] = {
        { RELEVO_68020, 0x00000003 },
        { RELEVO_68030, 0x00003313 },
        { RELEVO_68040, 0x80008000 },
        { RELEVO_68060, 0xF880E000 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        START(cases[i].model, 0x4E71);
        cache_write_cacr(cpu, 0xFFFFFFFF);
        CHECK_EQ(r->cacr, cases[i].kept);
        cache_write_cacr(cpu, 0);
        CHECK_EQ(r->cacr, 0);
        done();
    }
}
