/* SPDX-License-Identifier: MPL-2.0
 *
 * MULU, MULS, DIVU and DIVS (16-bit operand forms of the 68000).
 *
 * Their time depends on the operands. Multiply: 38 + 2n cycles (M68000UM
 * table 8-4), n counting the source's ones (MULU) or the changes between
 * neighbouring bits of the source with a 0 appended below it (MULS).
 * Divide: the cycle counts of the 68000's shift-and-subtract loop, as
 * worked out from its documented best and worst cases and checked against
 * the SingleStepTests data. The flags of an overflowing divide (undefined
 * in the manual) are those of the data.
 */
#include "ops_bits.h"

static unsigned popcount16(uint32_t v)
{
    unsigned n = 0;
    for (v &= 0xFFFF; v != 0; v &= v - 1)
        n++;
    return n;
}

/* ---- Multiply --------------------------------------------------------- */

static void op_mulu(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    uint32_t s = ea_read(c, &e);
    unsigned dn = (op >> 9) & 7;
    uint32_t v = (c->r.d[dn] & 0xFFFF) * s;
    c->r.d[dn] = v;
    set_nz(c, v, SZ_L);
    cycles(c, 34 + 2 * popcount16(s));
}

static void op_muls(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    uint32_t s = ea_read(c, &e);
    unsigned dn = (op >> 9) & 7;
    int32_t p = (int32_t)sext(c->r.d[dn], SZ_W) * (int32_t)sext(s, SZ_W);
    uint32_t v = (uint32_t)p;
    c->r.d[dn] = v;
    set_nz(c, v, SZ_L);
    cycles(c, 34 + 2 * popcount16(s ^ (s << 1)));
}

/* ---- Divide ----------------------------------------------------------- */

/* Division by zero: the trap, after 38 cycles plus the operand's. */
static void divide_by_zero(relevo_cpu *c)
{
    set_ccr_bits(c, CCR_C, 0);
    cycles(c, 4);
    cpu_exception(c, VEC_ZERO_DIVIDE, c->r.pc);
}

/* Overflow: V set, and in the data N set, Z and C clear. Dn stays. */
static void divide_overflow(relevo_cpu *c)
{
    set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
}

/* Quotient in the low word, remainder in the high one. */
static void divide_result(relevo_cpu *c, unsigned dn, uint32_t q, uint32_t r)
{
    c->r.d[dn] = (r & 0xFFFF) << 16 | (q & 0xFFFF);
    set_nz(c, q, SZ_W);
}

/* DIVU: 10 cycles on overflow; otherwise 76 plus up to 60 more, from the
 * quotient bits the loop works out (15 steps). */
static unsigned divu_cycles(uint32_t dividend, uint32_t divisor)
{
    unsigned t = 38;
    uint32_t hd = divisor << 16;
    for (int i = 0; i < 15; i++) {
        bool top = dividend & 0x80000000u;
        dividend <<= 1;
        if (top) {
            dividend -= hd;
        } else {
            t += 2;
            if (dividend >= hd) {
                dividend -= hd;
                t--;
            }
        }
    }
    return 2 * t;
}

static void op_divu(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    uint32_t s = ea_read(c, &e);
    unsigned dn = (op >> 9) & 7;
    uint32_t d = c->r.d[dn];
    if (s == 0) {
        divide_by_zero(c);
        return;
    }
    if ((d >> 16) >= s) {
        cycles(c, 10 - 4);
        divide_overflow(c);
        return;
    }
    cycles(c, divu_cycles(d, s) - 4);
    divide_result(c, dn, d / s, d % s);
}

static void op_divs(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    uint32_t s = ea_read(c, &e);
    unsigned dn = (op >> 9) & 7;
    uint32_t d = c->r.d[dn];
    if (s == 0) {
        divide_by_zero(c);
        return;
    }
    int32_t dividend = (int32_t)d;
    int32_t divisor = (int32_t)sext(s, SZ_W);
    uint32_t ad = dividend < 0 ? 0u - d : d;
    uint32_t as = divisor < 0 ? (uint32_t)-divisor : (uint32_t)divisor;

    unsigned t = dividend < 0 ? 7 : 6;
    if ((ad >> 16) >= as) {
        cycles(c, 2 * (t + 2) - 4);
        divide_overflow(c);
        return;
    }
    uint32_t aq = ad / as;
    t += 55;
    if (divisor >= 0)
        t += dividend < 0 ? 1 : -1;
    /* one step more for each of the top 15 quotient bits that is 0 */
    for (int i = 15; i >= 1; i--)
        if (!((aq >> i) & 1))
            t++;
    if (aq > 0x7FFF + (uint32_t)((dividend < 0) != (divisor < 0))) {
        /* the quotient does not fit a signed word */
        cycles(c, 2 * t - 4);
        divide_overflow(c);
        return;
    }
    cycles(c, 2 * t - 4);
    uint32_t ar = ad % as;
    uint32_t q = (dividend < 0) != (divisor < 0) ? 0u - aq : aq;
    uint32_t r = dividend < 0 ? 0u - ar : ar;
    divide_result(c, dn, q, r);
}

void ops_muldiv_init(op_table *tb)
{
    table_fill(tb, 0xC0C0, 0x0E3F, EA_DATA, op_mulu);
    table_fill(tb, 0xC1C0, 0x0E3F, EA_DATA, op_muls);
    table_fill(tb, 0x80C0, 0x0E3F, EA_DATA, op_divu);
    table_fill(tb, 0x81C0, 0x0E3F, EA_DATA, op_divs);
}
