/* SPDX-License-Identifier: MPL-2.0
 *
 * Integer instructions of the 68020 and later (M68000PRM chapter 4,
 * MC68020UM): MULU.L, MULS.L, DIVU.L, DIVS.L, DIVUL.L, DIVSL.L, EXTB.L,
 * CHK.L, CHK2, CMP2, CAS, CAS2, TRAPcc, PACK, UNPK, LINK.L, and CMPI and
 * TST with the operands the 68020 allows them. Bcc.L, BRA.L and BSR.L are
 * in ops_control.c; the 68020 addressing modes in ea.c.
 *
 * The 68060 traps MULx.L and DIVx.L with 64 bits, CHK2, CMP2 and CAS2 to
 * software ("unimplemented integer instruction"); here it runs them as the
 * 68040 does, which gives what that software would.
 */
#include "ops_020.h"

/* Register 0-15 of an extension word's bits 15-12: D0-D7, A0-A7. */
static uint32_t reg_value(relevo_cpu *c, unsigned n)
{
    return n < 8 ? c->r.d[n] : c->r.a[n - 8];
}

/* The flags of CMP: d - s, X unchanged. Returns the Z flag. */
static bool cmp_flags(relevo_cpu *c, uint32_t d, uint32_t s, unsigned size)
{
    uint32_t m = sz_mask(size), msb = sz_msb(size);
    d &= m;
    s &= m;
    uint32_t r = (d - s) & m;
    unsigned f = 0;
    if (r & msb)
        f |= CCR_N;
    if (r == 0)
        f |= CCR_Z;
    if ((s ^ d) & (r ^ d) & msb)
        f |= CCR_V;
    if (((s & ~d) | ((s | ~d) & r)) & msb)
        f |= CCR_C;
    set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
    return r == 0;
}

/* ---- MULU.L, MULS.L --------------------------------------------------- */

/* MULx.L <ea>,Dl (32 x 32 -> 32, V on overflow) or <ea>,Dh:Dl (-> 64).
 * Extension word: Dl in bits 14-12, signed in bit 11, 64-bit result in
 * bit 10, Dh in bits 2-0. N and Z come from the whole 64-bit product, also
 * for a 32-bit result that overflows; with Dh = Dl (undefined) the low half
 * stays. Both as FS-UAE, the bench's oracle. */
static void op_mul_l(relevo_cpu *c, uint16_t op)
{
    uint32_t ext = fetch16(c);
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    uint32_t s = ea_read(c, &e);
    unsigned dl = (ext >> 12) & 7, dh = ext & 7;
    bool sign = (ext & 0x0800) != 0;
    uint64_t p;
    if (sign)
        p = (uint64_t)((int64_t)(int32_t)c->r.d[dl] * (int32_t)s);
    else
        p = (uint64_t)c->r.d[dl] * s;

    unsigned f = 0;
    uint32_t lo = (uint32_t)p, hi = (uint32_t)(p >> 32);
    if (p == 0)
        f |= CCR_Z;
    if (hi & 0x80000000u)
        f |= CCR_N;
    if (ext & 0x0400) {
        c->r.d[dh] = hi;
        c->r.d[dl] = lo;
    } else {
        c->r.d[dl] = lo;
        /* the high half is not just the extension of the low one */
        uint32_t ext_hi = sign && (lo & 0x80000000u) ? 0xFFFFFFFFu : 0;
        if (hi != ext_hi)
            f |= CCR_V;
    }
    set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
    cycles(c, 40);
}

/* ---- DIVU.L, DIVS.L, DIVUL.L, DIVSL.L ---------------------------------- */

/* Extension word: Dq in bits 14-12, signed in bit 11, 64-bit dividend in
 * bit 10, Dr in bits 2-0.
 *
 *   bit 10 clear, Dr = Dq:  DIVx.L <ea>,Dq       32/32 -> 32q
 *   bit 10 clear, Dr != Dq: DIVxL.L <ea>,Dr:Dq   32/32 -> 32r:32q
 *   bit 10 set:             DIVx.L <ea>,Dr:Dq    64/32 -> 32r:32q
 *
 * The dividend is Dq (Dr:Dq for 64 bits). The remainder has the sign of
 * the dividend. When Dr = Dq only the quotient is kept. On overflow the
 * registers do not change; V is set, C cleared, N set and Z cleared (the
 * manuals leave N and Z undefined; FS-UAE, the bench's oracle, sets N too,
 * and Z in some cases only, by a rule not found yet). Division by zero
 * leaves the flags as they were (as FS-UAE). */
static void op_div_l(relevo_cpu *c, uint16_t op)
{
    uint32_t ext = fetch16(c);
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    uint32_t s = ea_read(c, &e);
    unsigned dq = (ext >> 12) & 7, dr = ext & 7;
    bool sign = (ext & 0x0800) != 0, wide = (ext & 0x0400) != 0;

    if (s == 0) {
        cycles(c, 4);
        cpu_exception(c, VEC_ZERO_DIVIDE, c->r.pc);
        return;
    }
    cycles(c, 70);

    uint64_t n = wide ? (uint64_t)c->r.d[dr] << 32 | c->r.d[dq]
                      : c->r.d[dq];
    uint32_t q, r;
    if (sign) {
        if (!wide)
            n = (uint64_t)(int64_t)(int32_t)n;
        bool nneg = (n >> 63) != 0, sneg = (s >> 31) != 0;
        /* magnitudes: 0 - n works for the most negative dividend too */
        uint64_t an = nneg ? 0 - n : n;
        uint64_t as = sneg ? 0 - (uint64_t)(int64_t)(int32_t)s : s;
        uint64_t aq = an / as, ar = an % as;
        bool qneg = nneg != sneg;
        if (aq > (qneg ? 0x80000000u : 0x7FFFFFFFu)) {
            set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
            return;
        }
        q = (uint32_t)(qneg ? 0 - aq : aq);
        r = (uint32_t)(nneg ? 0 - ar : ar);
    } else {
        uint64_t uq = n / s;
        if (uq >> 32) {
            set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
            return;
        }
        q = (uint32_t)uq;
        r = (uint32_t)(n % s);
    }
    c->r.d[dr] = r;
    c->r.d[dq] = q;
    set_nz(c, q, SZ_L);
}

/* ---- EXTB.L, LINK.L --------------------------------------------------- */

static void op_extb_l(relevo_cpu *c, uint16_t op)
{
    uint32_t v = sext(c->r.d[op & 7], SZ_B);
    c->r.d[op & 7] = v;
    set_nz(c, v, SZ_L);
}

/* LINK.L An,#d32: as LINK.W (A7 stores its value before the push). */
static void op_link_l(relevo_cpu *c, uint16_t op)
{
    unsigned n = op & 7;
    uint32_t d = fetch32(c);
    cpu_push32(c, c->r.a[n]);
    c->r.a[n] = c->r.a[7];
    c->r.a[7] += d;
    cycles(c, 2);
}

/* ---- CHK.L, CHK2, CMP2 ------------------------------------------------ */

/* CHK.W and CHK.L on the 68020 and later. The manuals define only N (set
 * if Dn < 0, clear if Dn > bound). As FS-UAE, the bench's oracle: Dn >
 * bound is checked first (so N is clear when both hold), and Z, V and C do
 * not change, nor does N when there is no trap. */
void chk_020(relevo_cpu *c, int32_t v, int32_t bound)
{
    if (v > bound) {
        set_ccr_bits(c, CCR_N, 0);
        cpu_exception(c, VEC_CHK, c->r.pc);
    } else if (v < 0) {
        set_ccr_bits(c, CCR_N, CCR_N);
        cpu_exception(c, VEC_CHK, c->r.pc);
    }
}

/* CHK.L <ea>,Dn: as CHK.W, on 32 bits. */
static void op_chk_l(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    int32_t bound = (int32_t)ea_read(c, &e);
    cycles(c, 4);
    chk_020(c, (int32_t)c->r.d[(op >> 9) & 7], bound);
}

/* CHK2/CMP2 <ea>,Rn: the lower bound at <ea>, the upper one after it.
 * Extension word: register in bits 15-12, CHK2 in bit 11.
 *
 * Bounds and register are taken sign-extended to 32 bits (for a data
 * register only its low byte or word counts; for an address register the
 * bounds are extended and the whole register compared, MC68020UM). Rn is
 * in bounds if Rn - lower <= upper - lower, unsigned: that is lower <= Rn
 * <= upper whether the program means them signed or unsigned, as long as
 * lower is the smaller one in its meaning. Z: Rn equals a bound. C: Rn is
 * out of bounds. N and V are left as they were (undefined). CHK2 traps
 * (vector 6) when C is set. */
static void op_chk2(relevo_cpu *c, uint16_t op)
{
    static const unsigned sizes[4] = { SZ_B, SZ_W, SZ_L, SZ_L };
    unsigned size = sizes[(op >> 9) & 3];
    uint32_t ext = fetch16(c);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t lo = sext(cpu_read(c, e.addr, size, e.acc), size);
    uint32_t hi = sext(cpu_read(c, e.addr + size, size, e.acc), size);
    unsigned rn = (ext >> 12) & 15;
    uint32_t v = reg_value(c, rn);
    if (rn < 8)
        v = sext(v, size);
    unsigned f = 0;
    if (v == lo || v == hi)
        f |= CCR_Z;
    else if (v - lo > hi - lo)
        f |= CCR_C;
    set_ccr_bits(c, CCR_Z | CCR_C, f);
    cycles(c, 8);
    if ((ext & 0x0800) && (f & CCR_C))
        cpu_exception(c, VEC_CHK, c->r.pc);
}

/* ---- CAS, CAS2 -------------------------------------------------------- */

static unsigned cas_size(uint16_t op)
{
    static const unsigned sizes[4] = { 0, SZ_B, SZ_W, SZ_L };
    return sizes[(op >> 9) & 3];
}

/* CAS Dc,Du,<ea>: compare <ea> with Dc; if equal, Du goes to <ea>,
 * otherwise <ea> goes to Dc. Extension word: Du in bits 8-6, Dc in 2-0. */
static void op_cas(relevo_cpu *c, uint16_t op)
{
    unsigned size = cas_size(op);
    uint32_t ext = fetch16(c);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t m = ea_read(c, &e);
    unsigned dc = ext & 7, du = (ext >> 6) & 7;
    cycles(c, 4);
    if (cmp_flags(c, m, c->r.d[dc], size))
        ea_write(c, &e, c->r.d[du]);
    else
        set_dreg(c, dc, m, size);
}

/* CAS2 Dc1:Dc2,Du1:Du2,(Rn1):(Rn2), word or long. The flags are those of
 * the first comparison, or of the second when the first is equal. Both
 * equal: Du1 to (Rn1), then Du2 to (Rn2). Otherwise both memory operands
 * go to Dc1 and Dc2; with Dc1 = Dc2 the first operand is kept. */
static void op_cas2(relevo_cpu *c, uint16_t op)
{
    unsigned size = cas_size(op);
    uint32_t e1 = fetch16(c), e2 = fetch16(c);
    uint32_t a1 = reg_value(c, (e1 >> 12) & 15);
    uint32_t a2 = reg_value(c, (e2 >> 12) & 15);
    unsigned dc1 = e1 & 7, du1 = (e1 >> 6) & 7;
    unsigned dc2 = e2 & 7, du2 = (e2 >> 6) & 7;
    uint32_t m1 = cpu_read(c, a1, size, ACC_DATA);
    uint32_t m2 = cpu_read(c, a2, size, ACC_DATA);
    cycles(c, 8);
    if (cmp_flags(c, m1, c->r.d[dc1], size)
        && cmp_flags(c, m2, c->r.d[dc2], size)) {
        cpu_write(c, a1, c->r.d[du1], size);
        cpu_write(c, a2, c->r.d[du2], size);
    } else {
        set_dreg(c, dc2, m2, size);
        set_dreg(c, dc1, m1, size);
    }
}

/* ---- TRAPcc ----------------------------------------------------------- */

/* TRAPcc, TRAPcc.W #d16, TRAPcc.L #d32: the operand is only for the
 * handler to read. If the condition holds, the TRAPV exception (vector 7)
 * with the address of the next instruction. */
static void op_trapcc(relevo_cpu *c, uint16_t op)
{
    if ((op & 7) == 2)
        fetch16(c);
    else if ((op & 7) == 3)
        fetch32(c);
    if (cond_true(c->r.sr, (op >> 8) & 15))
        cpu_exception(c, VEC_TRAPV, c->r.pc);
}

/* ---- PACK, UNPK ------------------------------------------------------- */

/* -(An) for one byte: A7 stays even. */
static uint32_t predec_byte(relevo_cpu *c, unsigned n)
{
    c->r.a[n] -= n == 7 ? 2 : 1;
    return c->r.a[n];
}

/* PACK Dx,Dy,#adj or PACK -(Ax),-(Ay),#adj: the word (two unpacked BCD
 * digits in bits 11-8 and 3-0) plus the adjustment, packed into a byte.
 * From memory the word is two bytes, the low one read first. Flags
 * unchanged. */
static void op_pack(relevo_cpu *c, uint16_t op)
{
    uint32_t adj = fetch16(c);
    unsigned x = op & 7, y = (op >> 9) & 7;
    uint32_t v;
    if (op & 8) {
        uint32_t lo = cpu_read8(c, predec_byte(c, x), ACC_DATA);
        uint32_t hi = cpu_read8(c, predec_byte(c, x), ACC_DATA);
        v = hi << 8 | lo;
    } else {
        v = c->r.d[x];
    }
    v = (v + adj) & 0xFFFF;
    uint32_t b = ((v >> 4) & 0xF0) | (v & 0x0F);
    if (op & 8)
        cpu_write8(c, predec_byte(c, y), b);
    else
        set_dreg(c, y, b, SZ_B);
    cycles(c, 4);
}

/* UNPK Dx,Dy,#adj or UNPK -(Ax),-(Ay),#adj: the two digits of a byte
 * spread to bits 11-8 and 3-0, plus the adjustment. To memory the low
 * byte goes first. Flags unchanged. */
static void op_unpk(relevo_cpu *c, uint16_t op)
{
    uint32_t adj = fetch16(c);
    unsigned x = op & 7, y = (op >> 9) & 7;
    uint32_t b = (op & 8) ? cpu_read8(c, predec_byte(c, x), ACC_DATA)
                          : c->r.d[x] & 0xFF;
    uint32_t v = ((((b & 0xF0) << 4) | (b & 0x0F)) + adj) & 0xFFFF;
    if (op & 8) {
        cpu_write8(c, predec_byte(c, y), v & 0xFF);
        cpu_write8(c, predec_byte(c, y), v >> 8);
    } else {
        set_dreg(c, y, v, SZ_W);
    }
    cycles(c, 4);
}

/* ---- CMPI and TST with the 68020's operands --------------------------- */

/* CMPI #imm,<ea> with a PC-relative <ea>. */
static void op_cmpi_pc(relevo_cpu *c, uint16_t op)
{
    unsigned size = size_field(op);
    uint32_t s = size == SZ_L ? fetch32(c) : fetch16(c) & sz_mask(size);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    cmp_flags(c, ea_read(c, &e), s, size);
}

/* TST <ea> with An (word and long), PC-relative or immediate <ea>. */
static void op_tst_020(relevo_cpu *c, uint16_t op)
{
    unsigned size = size_field(op);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    set_nz(c, ea_read(c, &e), size);
}

/* ---- Table ------------------------------------------------------------ */

void ops_020_init(op_table *tb)
{
    if (tb->model < RELEVO_68020)
        return;

    table_fill(tb, 0x4C00, 0x003F, EA_DATA, op_mul_l);
    table_fill(tb, 0x4C40, 0x003F, EA_DATA, op_div_l);
    table_fill(tb, 0x49C0, 0x0007, 0, op_extb_l);
    table_fill(tb, 0x4808, 0x0007, 0, op_link_l);
    table_fill(tb, 0x4100, 0x0E3F, EA_DATA, op_chk_l);
    for (uint16_t s = 0; s < 3; s++) {
        table_fill(tb, (uint16_t)(0x00C0 | s << 9), 0x003F, EA_CONTROL,
                   op_chk2);
        table_fill(tb, (uint16_t)(0x0AC0 + (s << 9)), 0x003F, EA_MEM_ALT,
                   op_cas);
    }
    table_set(tb, 0x0CFC, op_cas2);
    table_set(tb, 0x0EFC, op_cas2);
    for (uint16_t cc = 0; cc < 16; cc++)
        for (uint16_t mode = 2; mode <= 4; mode++)
            table_set(tb, (uint16_t)(0x50F8 | cc << 8 | mode), op_trapcc);
    table_fill(tb, 0x8140, 0x0E0F, 0, op_pack);
    table_fill(tb, 0x8180, 0x0E0F, 0, op_unpk);

    for (uint16_t s = 0; s < 3; s++) {
        table_fill(tb, (uint16_t)(0x0C00 | s << 6), 0x003F,
                   EAB_PCD16 | EAB_PCD8, op_cmpi_pc);
        unsigned tst = EAB_PCD16 | EAB_PCD8 | EAB_IMM
                     | (s != 0 ? EAB_AREG : 0);
        table_fill(tb, (uint16_t)(0x4A00 | s << 6), 0x003F, tst, op_tst_020);
    }

    ops_020_bf_init(tb);
    ops_020_cache_init(tb);
}
