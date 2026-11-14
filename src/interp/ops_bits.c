/* SPDX-License-Identifier: MPL-2.0
 *
 * Shifts and rotates (ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR), bit
 * operations (BTST, BCHG, BCLR, BSET) and BCD arithmetic (ABCD, SBCD,
 * NBCD). Multiply and divide are in ops_muldiv.c.
 *
 * Cycles follow the M68000UM timing tables; the flags the manuals leave
 * undefined (N and V of the BCD instructions) are those of the
 * SingleStepTests/m68000 data.
 */
#include "ops_bits.h"

/* ---- Shifts and rotates ----------------------------------------------- */

enum { SH_AS = 0, SH_LS = 1, SH_ROX = 2, SH_RO = 3 };

/* Shifts or rotates the `size`-byte value v by `count` (0-63) and sets the
 * flags (M68000PRM chapter 4). Returns the result. */
static uint32_t shift(relevo_cpu *c, unsigned type, bool left, uint32_t v,
                      unsigned count, unsigned size)
{
    unsigned bits = 8 * size;
    uint64_t mask = sz_mask(size);
    uint64_t x = v & mask;
    uint64_t res = x;
    bool carry = false, overflow = false;
    bool xflag = (c->r.sr & CCR_X) != 0;
    bool set_x = count != 0;    /* X is untouched by a count of zero */

    switch (type) {
    case SH_AS:
    case SH_LS:
        if (count == 0)
            break;
        if (left) {
            if (count <= bits)
                carry = (x >> (bits - count)) & 1;
            res = count < bits ? (x << count) & mask : 0;
            if (type == SH_AS) {
                /* V: the sign bit changed at some point, that is, the
                 * top count + 1 bits were not all equal. */
                if (count >= bits) {
                    overflow = x != 0;
                } else {
                    uint64_t top = mask & ~(mask >> (count + 1));
                    overflow = (x & top) != 0 && (x & top) != top;
                }
            }
        } else if (type == SH_LS) {
            if (count <= bits)
                carry = (x >> (count - 1)) & 1;
            res = count < bits ? x >> count : 0;
        } else {
            bool neg = (x >> (bits - 1)) & 1;
            uint64_t ext = neg ? x | ~mask : x;     /* sign-extended */
            unsigned n = count < bits ? count : bits;
            carry = (ext >> (n - 1)) & 1;
            res = (count < bits ? ext >> count : (neg ? ~0ull : 0)) & mask;
        }
        if (set_x)
            xflag = carry;
        break;
    case SH_RO: {
        set_x = false;
        if (count == 0)
            break;
        unsigned n = count % bits;
        if (left)
            res = ((x << n) | (x >> (bits - n))) & mask;
        else
            res = ((x >> n) | (x << (bits - n))) & mask;
        carry = left ? res & 1 : (res >> (bits - 1)) & 1;
        break;
    }
    default: {  /* ROX: rotate through X, bits + 1 wide */
        unsigned n = count % (bits + 1);
        uint64_t w = x | (uint64_t)xflag << bits;
        uint64_t wm = (mask << 1) | 1;
        if (n != 0) {
            if (left)
                w = ((w << n) | (w >> (bits + 1 - n))) & wm;
            else
                w = ((w >> n) | (w << (bits + 1 - n))) & wm;
        }
        res = w & mask;
        xflag = (w >> bits) & 1;
        carry = xflag;          /* C = X, also for a count of zero */
        set_x = true;
        break;
    }
    }

    unsigned f = 0;
    if (res == 0)
        f |= CCR_Z;
    if ((res >> (bits - 1)) & 1)
        f |= CCR_N;
    if (overflow)
        f |= CCR_V;
    if (carry)
        f |= CCR_C;
    unsigned m = CCR_N | CCR_Z | CCR_V | CCR_C;
    if (set_x) {
        m |= CCR_X;
        if (xflag)
            f |= CCR_X;
    }
    set_ccr_bits(c, m, f);
    return (uint32_t)res;
}

/* <op> #n,Dy / <op> Dx,Dy: 1110 ccc d ss i tt yyy. 6 + 2n cycles for bytes
 * and words, 8 + 2n for longs. */
static void op_shift_reg(relevo_cpu *c, uint16_t op)
{
    unsigned size = size_field(op);
    unsigned ccc = (op >> 9) & 7;
    unsigned count = (op & 0x20) ? c->r.d[ccc] & 63 : (ccc ? ccc : 8);
    unsigned dy = op & 7;
    uint32_t v = shift(c, (op >> 3) & 3, (op & 0x100) != 0, c->r.d[dy],
                       count, size);
    set_dreg(c, dy, v, size);
    cycles(c, (size == SZ_L ? 4 : 2) + 2 * count);
}

/* <op> <ea>: a word in memory, shifted by one. */
static void op_shift_mem(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    uint32_t v = ea_read(c, &e);
    ea_write(c, &e, shift(c, (op >> 9) & 3, (op & 0x100) != 0, v, 1, SZ_W));
}

/* ---- Bit operations --------------------------------------------------- */

enum { BIT_TST = 0, BIT_CHG = 1, BIT_CLR = 2, BIT_SET = 3 };

/* Bit `n` of the operand: modulo 32 in a data register (long), modulo 8 in
 * memory (byte). Z is the bit's old value inverted. */
static void bit_op(relevo_cpu *c, uint16_t op, unsigned n)
{
    unsigned type = (op >> 6) & 3;
    ea_t e;
    unsigned mode = (op >> 3) & 7;
    ea_decode_op(c, &e, op, mode == 0 ? SZ_L : SZ_B);
    uint32_t v = ea_read(c, &e);
    n &= mode == 0 ? 31 : 7;
    uint32_t bit = 1u << n;
    set_ccr_bits(c, CCR_Z, (v & bit) ? 0 : CCR_Z);
    switch (type) {
    case BIT_CHG: v ^= bit; break;
    case BIT_CLR: v &= ~bit; break;
    case BIT_SET: v |= bit; break;
    default: break;
    }
    if (mode == 0) {
        /* BTST 6, BCHG/BSET 6 or 8, BCLR 8 or 10 (bits 16-31 take 2
         * more); with an immediate bit number, 4 more for its word. */
        unsigned t = 2;
        if (type != BIT_TST)
            t += (type == BIT_CLR ? 2 : 0) + (n >= 16 ? 2 : 0);
        cycles(c, t);
    } else if (e.kind == EA_IMM) {
        cycles(c, 2);       /* BTST Dn,#imm: 10, not the 8 of 4 + <ea> */
    }
    if (type != BIT_TST)
        ea_write(c, &e, v);
}

/* B<op> Dn,<ea>: 0000 nnn 1tt eeeeee */
static void op_bit_dyn(relevo_cpu *c, uint16_t op)
{
    bit_op(c, op, c->r.d[(op >> 9) & 7]);
}

/* B<op> #n,<ea>: 0000 1000 tt eeeeee, then the bit number. */
static void op_bit_imm(relevo_cpu *c, uint16_t op)
{
    unsigned n = fetch16(c) & 0xFF;
    bit_op(c, op, n);
}

/* ---- BCD -------------------------------------------------------------- */

/* d + s + X in BCD. The decimal carry decides on the low-corrected sum;
 * V is set when the correction turns bit 7 on (undefined in the manual,
 * as in the data); N is bit 7 of the result. */
static uint32_t bcd_add(relevo_cpu *c, uint32_t s, uint32_t d)
{
    unsigned x = (c->r.sr & CCR_X) ? 1 : 0;
    unsigned bin = s + d + x;
    unsigned res = bin;
    if ((s & 0xF) + (d & 0xF) + x > 9)
        res += 6;
    bool carry = res > 0x9F;
    if (carry)
        res += 0x60;
    unsigned f = (res & 0x80) ? CCR_N : 0;
    if (~bin & res & 0x80)
        f |= CCR_V;
    if (carry)
        f |= CCR_C | CCR_X;
    /* Z: cleared if the result is nonzero, else unchanged */
    unsigned m = CCR_X | CCR_N | CCR_V | CCR_C | ((res & 0xFF) ? CCR_Z : 0);
    set_ccr_bits(c, m, f);
    return res & 0xFF;
}

/* d - s - X in BCD. The borrow (C, X) is that of the low-corrected
 * difference; the high correction is applied on the binary borrow. V is
 * set when the correction turns bit 7 off. */
static uint32_t bcd_sub(relevo_cpu *c, uint32_t s, uint32_t d)
{
    int x = (c->r.sr & CCR_X) ? 1 : 0;
    int bin = (int)d - (int)s - x;
    int res = bin;
    if ((int)(d & 0xF) - (int)(s & 0xF) - x < 0)
        res -= 6;
    bool borrow = res < 0;
    if (bin < 0)
        res -= 0x60;
    unsigned r = (unsigned)res & 0xFF;
    unsigned f = (r & 0x80) ? CCR_N : 0;
    if ((unsigned)bin & ~(unsigned)res & 0x80)
        f |= CCR_V;
    if (borrow)
        f |= CCR_C | CCR_X;
    /* Z as in bcd_add() */
    unsigned m = CCR_X | CCR_N | CCR_V | CCR_C | (r ? CCR_Z : 0);
    set_ccr_bits(c, m, f);
    return r;
}

/* ABCD/SBCD Dy,Dx and -(Ay),-(Ax): 1100/1000 xxx 10000 r yyy */
static void bcd_xy(relevo_cpu *c, uint16_t op, bool add)
{
    unsigned x = (op >> 9) & 7, y = op & 7;
    if (!(op & 8)) {
        uint32_t s = c->r.d[y] & 0xFF, d = c->r.d[x] & 0xFF;
        set_dreg(c, x, add ? bcd_add(c, s, d) : bcd_sub(c, s, d), SZ_B);
        cycles(c, 2);
        return;
    }
    /* -(Ay),-(Ax): 18 cycles, one internal predecrement step */
    cycles(c, 2);
    c->ae_pc = c->r.pc;
    c->r.a[y] -= y == 7 ? 2 : 1;
    uint32_t s = cpu_read8(c, c->r.a[y], ACC_DATA);
    c->r.a[x] -= x == 7 ? 2 : 1;
    uint32_t d = cpu_read8(c, c->r.a[x], ACC_DATA);
    cpu_write8(c, c->r.a[x], add ? bcd_add(c, s, d) : bcd_sub(c, s, d));
}

static void op_abcd(relevo_cpu *c, uint16_t op) { bcd_xy(c, op, true); }
static void op_sbcd(relevo_cpu *c, uint16_t op) { bcd_xy(c, op, false); }

/* NBCD <ea>: 0 - <ea> - X */
static void op_nbcd(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_B);
    uint32_t v = ea_read(c, &e);
    if (e.kind == EA_DREG)
        cycles(c, 2);
    ea_write(c, &e, bcd_sub(c, v, 0));
}

/* ---- Table ------------------------------------------------------------ */

void ops_bits_init(op_table *tb)
{
    /* Shifts and rotates on registers: 1110 ccc d ss i tt yyy, ss != 11 */
    for (uint16_t s = 0; s < 3; s++)
        table_fill(tb, (uint16_t)(0xE000 | s << 6), 0x0F3F, 0, op_shift_reg);
    /* On memory: 1110 0tt d 11 eeeeee */
    table_fill(tb, 0xE0C0, 0x073F, EA_MEM_ALT, op_shift_mem);

    /* BTST Dn,<ea> takes any data mode, #imm included; the others data
     * alterable ones. BTST #n,<ea>: data modes but #imm. */
    table_fill(tb, 0x0100, 0x0E3F, EA_DATA, op_bit_dyn);
    table_fill(tb, 0x0800, 0x003F, EA_DATA & ~(unsigned)EAB_IMM, op_bit_imm);
    for (uint16_t t = BIT_CHG; t <= BIT_SET; t++) {
        table_fill(tb, (uint16_t)(0x0100 | t << 6), 0x0E3F, EA_DATA_ALT,
                   op_bit_dyn);
        table_fill(tb, (uint16_t)(0x0800 | t << 6), 0x003F, EA_DATA_ALT,
                   op_bit_imm);
    }

    table_fill(tb, 0xC100, 0x0E0F, 0, op_abcd);
    table_fill(tb, 0x8100, 0x0E0F, 0, op_sbcd);
    table_fill(tb, 0x4800, 0x003F, EA_DATA_ALT, op_nbcd);

    ops_muldiv_init(tb);
}
