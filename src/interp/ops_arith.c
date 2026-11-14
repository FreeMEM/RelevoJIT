/* SPDX-License-Identifier: MPL-2.0
 *
 * Arithmetic and logic (task R2b): ADD, ADDA, ADDI, ADDQ, ADDX, SUB, SUBA,
 * SUBI, SUBQ, SUBX, CMP, CMPA, CMPI, CMPM, AND, ANDI, OR, ORI, EOR, EORI,
 * NEG, NEGX and NOT. (ANDI, ORI and EORI to CCR and SR are in
 * ops_system.c.)
 *
 * Cycles: the 68000 times of M68000PRM chapter 8 minus 4 per word and per
 * access, which count themselves (see interp.h).
 */
#include "interp.h"

/* ---- Flags ------------------------------------------------------------ */

enum alu_op { ALU_ADD, ALU_SUB, ALU_CMP, ALU_AND, ALU_OR, ALU_EOR };

/* d + s + x: sets X, N, Z, V and C. With `sticky_z` (ADDX), Z is only
 * cleared, never set. */
static uint32_t add_flags(relevo_cpu *c, uint32_t d, uint32_t s, uint32_t x,
                          unsigned size, bool sticky_z)
{
    uint32_t m = sz_mask(size), msb = sz_msb(size);
    d &= m;
    s &= m;
    uint32_t r = (d + s + x) & m;
    unsigned f = 0;
    if (r & msb)
        f |= CCR_N;
    if ((s ^ r) & (d ^ r) & msb)
        f |= CCR_V;
    /* carry out of the top bit */
    if (((s & d) | ((s | d) & ~r)) & msb)
        f |= CCR_C | CCR_X;
    if (sticky_z) {
        f |= (r == 0) ? (c->r.sr & CCR_Z) : 0;
    } else if (r == 0) {
        f |= CCR_Z;
    }
    set_ccr_bits(c, CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C, f);
    return r;
}

/* d - s - x: sets N, Z, V and C, and X too unless `cmp`. With `sticky_z`
 * (SUBX, NEGX), Z is only cleared, never set. */
static uint32_t sub_flags(relevo_cpu *c, uint32_t d, uint32_t s, uint32_t x,
                          unsigned size, bool cmp, bool sticky_z)
{
    uint32_t m = sz_mask(size), msb = sz_msb(size);
    d &= m;
    s &= m;
    uint32_t r = (d - s - x) & m;
    unsigned f = 0;
    if (r & msb)
        f |= CCR_N;
    if ((s ^ d) & (r ^ d) & msb)
        f |= CCR_V;
    /* borrow into the top bit */
    if (((s & ~d) | ((s | ~d) & r)) & msb)
        f |= CCR_C | CCR_X;
    if (sticky_z) {
        f |= (r == 0) ? (c->r.sr & CCR_Z) : 0;
    } else if (r == 0) {
        f |= CCR_Z;
    }
    unsigned mask = CCR_N | CCR_Z | CCR_V | CCR_C | (cmp ? 0 : CCR_X);
    set_ccr_bits(c, mask, f);
    return r;
}

/* The operation of the two-operand instructions, with their flags. */
static uint32_t alu(relevo_cpu *c, unsigned op, uint32_t d, uint32_t s,
                    unsigned size)
{
    uint32_t r;
    switch (op) {
    case ALU_ADD: return add_flags(c, d, s, 0, size, false);
    case ALU_SUB: return sub_flags(c, d, s, 0, size, false, false);
    case ALU_CMP: return sub_flags(c, d, s, 0, size, true, false);
    case ALU_AND: r = d & s; break;
    case ALU_OR:  r = d | s; break;
    default:      r = d ^ s; break;
    }
    set_nz(c, r, size);
    return r;
}

/* ---- <ea>,Dn and Dn,<ea> ---------------------------------------------- */

/* ADD, SUB, AND, OR, CMP <ea>,Dn. Long takes 2 internal cycles more, 4
 * with a register or immediate source (CMP: always 2). */
static void ea_to_dreg(relevo_cpu *c, uint16_t op, unsigned alu_op)
{
    unsigned size = size_field(op), dn = (op >> 9) & 7;
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t s = ea_read(c, &e);
    uint32_t r = alu(c, alu_op, c->r.d[dn], s, size);
    if (alu_op != ALU_CMP)
        set_dreg(c, dn, r, size);
    if (size == SZ_L)
        cycles(c, (alu_op == ALU_CMP || e.kind == EA_MEM) ? 2 : 4);
}

/* ADD, SUB, AND, OR, EOR Dn,<ea>: read, change, write. EOR also comes
 * here with a data register destination (long: 4 internal cycles). */
static void dreg_to_ea(relevo_cpu *c, uint16_t op, unsigned alu_op)
{
    unsigned size = size_field(op), dn = (op >> 9) & 7;
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t d = ea_read(c, &e);
    uint32_t r = alu(c, alu_op, d, c->r.d[dn], size);
    if (e.kind == EA_DREG && size == SZ_L)
        cycles(c, 4);
    ea_write(c, &e, r);
}

static void op_add_to_d(relevo_cpu *c, uint16_t op) { ea_to_dreg(c, op, ALU_ADD); }
static void op_sub_to_d(relevo_cpu *c, uint16_t op) { ea_to_dreg(c, op, ALU_SUB); }
static void op_and_to_d(relevo_cpu *c, uint16_t op) { ea_to_dreg(c, op, ALU_AND); }
static void op_or_to_d(relevo_cpu *c, uint16_t op)  { ea_to_dreg(c, op, ALU_OR); }
static void op_cmp(relevo_cpu *c, uint16_t op)      { ea_to_dreg(c, op, ALU_CMP); }
static void op_add_to_ea(relevo_cpu *c, uint16_t op) { dreg_to_ea(c, op, ALU_ADD); }
static void op_sub_to_ea(relevo_cpu *c, uint16_t op) { dreg_to_ea(c, op, ALU_SUB); }
static void op_and_to_ea(relevo_cpu *c, uint16_t op) { dreg_to_ea(c, op, ALU_AND); }
static void op_or_to_ea(relevo_cpu *c, uint16_t op)  { dreg_to_ea(c, op, ALU_OR); }
static void op_eor(relevo_cpu *c, uint16_t op)       { dreg_to_ea(c, op, ALU_EOR); }

/* ---- ADDA, SUBA, CMPA ------------------------------------------------- */

/* Opmode 011 is word (sign-extended), 111 long. The whole An changes; no
 * flags. */
static uint32_t read_addr_src(relevo_cpu *c, uint16_t op, ea_t *e)
{
    unsigned size = (op & 0x0100) ? SZ_L : SZ_W;
    ea_decode_op(c, e, op, size);
    return sext(ea_read(c, e), size);
}

/* ADDA/SUBA: word 4 internal cycles; long 2, 4 with a register or
 * immediate source. */
static void adda_cycles(relevo_cpu *c, uint16_t op, const ea_t *e)
{
    cycles(c, (!(op & 0x0100) || e->kind != EA_MEM) ? 4 : 2);
}

static void op_adda(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    uint32_t s = read_addr_src(c, op, &e);
    c->r.a[(op >> 9) & 7] += s;
    adda_cycles(c, op, &e);
}

static void op_suba(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    uint32_t s = read_addr_src(c, op, &e);
    c->r.a[(op >> 9) & 7] -= s;
    adda_cycles(c, op, &e);
}

static void op_cmpa(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    uint32_t s = read_addr_src(c, op, &e);
    sub_flags(c, c->r.a[(op >> 9) & 7], s, 0, SZ_L, true, false);
    cycles(c, 2);
}

/* ---- Immediate: ADDI, SUBI, CMPI, ANDI, ORI, EORI ---------------------- */

static void immediate(relevo_cpu *c, uint16_t op, unsigned alu_op)
{
    unsigned size = size_field(op);
    uint32_t s = size == SZ_L ? fetch32(c) : fetch16(c) & sz_mask(size);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t d = ea_read(c, &e);
    uint32_t r = alu(c, alu_op, d, s, size);
    if (e.kind == EA_DREG && size == SZ_L)
        cycles(c, alu_op == ALU_CMP ? 2 : 4);
    if (alu_op != ALU_CMP)
        ea_write(c, &e, r);
}

static void op_addi(relevo_cpu *c, uint16_t op) { immediate(c, op, ALU_ADD); }
static void op_subi(relevo_cpu *c, uint16_t op) { immediate(c, op, ALU_SUB); }
static void op_cmpi(relevo_cpu *c, uint16_t op) { immediate(c, op, ALU_CMP); }
static void op_andi(relevo_cpu *c, uint16_t op) { immediate(c, op, ALU_AND); }
static void op_ori(relevo_cpu *c, uint16_t op)  { immediate(c, op, ALU_OR); }
static void op_eori(relevo_cpu *c, uint16_t op) { immediate(c, op, ALU_EOR); }

/* ---- ADDQ, SUBQ ------------------------------------------------------- */

/* #1-8 (0 is 8). To An, the whole register changes and the flags do not;
 * Dn.L and An take 4 internal cycles. */
static void quick(relevo_cpu *c, uint16_t op, bool sub)
{
    unsigned size = size_field(op);
    uint32_t q = (op >> 9) & 7;
    if (q == 0)
        q = 8;
    if (((op >> 3) & 7) == 1) {
        unsigned an = op & 7;
        c->r.a[an] = sub ? c->r.a[an] - q : c->r.a[an] + q;
        cycles(c, 4);
        return;
    }
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t d = ea_read(c, &e);
    uint32_t r = alu(c, sub ? ALU_SUB : ALU_ADD, d, q, size);
    if (e.kind == EA_DREG && size == SZ_L)
        cycles(c, 4);
    ea_write(c, &e, r);
}

static void op_addq(relevo_cpu *c, uint16_t op) { quick(c, op, false); }
static void op_subq(relevo_cpu *c, uint16_t op) { quick(c, op, true); }

/* ---- ADDX, SUBX ------------------------------------------------------- */

/* -(An) for ADDX and SUBX. A long is read low word first, at An-2, and An
 * only changes once both words are in (SingleStepTests data). */
static uint32_t read_predec(relevo_cpu *c, unsigned reg, unsigned size)
{
    uint32_t addr = c->r.a[reg];
    if (size == SZ_L) {
        uint32_t lo = cpu_read16(c, addr - 2, ACC_DATA);
        uint32_t hi = cpu_read16(c, addr - 4, ACC_DATA);
        c->r.a[reg] = addr - 4;
        return hi << 16 | lo;
    }
    addr -= (size == SZ_B && reg == 7) ? 2 : size;
    c->r.a[reg] = addr;
    return cpu_read(c, addr, size, ACC_DATA);
}

static void extended(relevo_cpu *c, uint16_t op, bool sub)
{
    unsigned size = size_field(op), rx = (op >> 9) & 7, ry = op & 7;
    uint32_t x = (c->r.sr & CCR_X) ? 1 : 0;
    if (!(op & 8)) {
        /* Dy,Dx */
        uint32_t d = c->r.d[rx], s = c->r.d[ry];
        uint32_t r = sub ? sub_flags(c, d, s, x, size, false, true)
                         : add_flags(c, d, s, x, size, true);
        set_dreg(c, rx, r, size);
        if (size == SZ_L)
            cycles(c, 4);
        return;
    }
    /* -(Ay),-(Ax): 2 internal cycles, then the reads; a fault stacks the
     * PC one word on. */
    cycles(c, 2);
    c->ae_pc = c->r.pc + 2;
    uint32_t s = read_predec(c, ry, size);
    uint32_t d = read_predec(c, rx, size);
    uint32_t r = sub ? sub_flags(c, d, s, x, size, false, true)
                     : add_flags(c, d, s, x, size, true);
    uint32_t addr = c->r.a[rx];
    if (size == SZ_L) {
        cpu_write16(c, addr + 2, r & 0xFFFF);
        cpu_write16(c, addr, r >> 16);
    } else {
        cpu_write(c, addr, r, size);
    }
}

static void op_addx(relevo_cpu *c, uint16_t op) { extended(c, op, false); }
static void op_subx(relevo_cpu *c, uint16_t op) { extended(c, op, true); }

/* ---- CMPM (Ay)+,(Ax)+ ------------------------------------------------- */

/* If a read faults (SingleStepTests data): the stacked PC is one word on;
 * Ay has moved by 2 (word and long alike), Ax has not moved. */
static void op_cmpm(relevo_cpu *c, uint16_t op)
{
    unsigned size = size_field(op), ry = op & 7, rx = (op >> 9) & 7;
    unsigned step = (size == SZ_B) ? 1 : size;
    c->ae_pc = c->r.pc + 2;
    uint32_t addr = c->r.a[ry];
    if (size != SZ_B && (addr & 1))
        c->r.a[ry] += 2;
    if (size == SZ_B && ry == 7)
        step = 2;
    uint32_t s = cpu_read(c, addr, size, ACC_DATA);
    c->r.a[ry] = addr + step;
    step = (size == SZ_B && rx == 7) ? 2 : size;
    addr = c->r.a[rx];
    uint32_t d = cpu_read(c, addr, size, ACC_DATA);
    c->r.a[rx] = addr + step;
    sub_flags(c, d, s, 0, size, true, false);
}

/* ---- NEG, NEGX, NOT --------------------------------------------------- */

/* Dn.L takes 2 internal cycles. */
static void unary(relevo_cpu *c, uint16_t op, unsigned kind)
{
    unsigned size = size_field(op);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t d = ea_read(c, &e);
    uint32_t r;
    switch (kind) {
    case 0:     /* NEGX */
        r = sub_flags(c, 0, d, (c->r.sr & CCR_X) ? 1 : 0, size, false, true);
        break;
    case 1:     /* NEG */
        r = sub_flags(c, 0, d, 0, size, false, false);
        break;
    default:    /* NOT */
        r = ~d;
        set_nz(c, r, size);
        break;
    }
    if (e.kind == EA_DREG && size == SZ_L)
        cycles(c, 2);
    ea_write(c, &e, r);
}

static void op_negx(relevo_cpu *c, uint16_t op) { unary(c, op, 0); }
static void op_neg(relevo_cpu *c, uint16_t op)  { unary(c, op, 1); }
static void op_not(relevo_cpu *c, uint16_t op)  { unary(c, op, 2); }

/* ---- Table ------------------------------------------------------------ */

/* The line 8/9/B/C/D layout: register in bits 11-9, opmode in 8-6. */
static void fill_line(op_table *tb, uint16_t line, unsigned opmode,
                      unsigned classes, relevo_handler h)
{
    table_fill(tb, (uint16_t)(line | opmode << 6), 0x0E3F, classes, h);
}

void ops_arith_init(op_table *tb)
{
    static const struct {
        uint16_t line;
        relevo_handler to_d, to_ea;
    } lines[] = {
        { 0xD000, op_add_to_d, op_add_to_ea },
        { 0x9000, op_sub_to_d, op_sub_to_ea },
        { 0xC000, op_and_to_d, op_and_to_ea },
        { 0x8000, op_or_to_d, op_or_to_ea },
    };
    for (unsigned i = 0; i < 4; i++) {
        bool arith = i < 2;
        for (unsigned sz = 0; sz < 3; sz++) {
            unsigned src = (arith && sz != 0) ? EA_ALL : EA_DATA;
            fill_line(tb, lines[i].line, sz, src, lines[i].to_d);
            fill_line(tb, lines[i].line, 4 + sz, EA_MEM_ALT, lines[i].to_ea);
        }
    }
    for (unsigned sz = 0; sz < 3; sz++) {
        fill_line(tb, 0xB000, sz, sz ? EA_ALL : EA_DATA, op_cmp);
        fill_line(tb, 0xB000, 4 + sz, EA_DATA_ALT, op_eor);
        /* ADDX/SUBX: Dy,Dx and -(Ay),-(Ax); CMPM (Ay)+,(Ax)+ */
        uint16_t x = (uint16_t)(0x0100 | sz << 6);
        table_fill(tb, (uint16_t)(0xD000 | x), 0x0E0F, 0, op_addx);
        table_fill(tb, (uint16_t)(0x9000 | x), 0x0E0F, 0, op_subx);
        table_fill(tb, (uint16_t)(0xB008 | x), 0x0E07, 0, op_cmpm);

        uint16_t s = (uint16_t)(sz << 6);
        table_fill(tb, (uint16_t)(0x0000 | s), 0x003F, EA_DATA_ALT, op_ori);
        table_fill(tb, (uint16_t)(0x0200 | s), 0x003F, EA_DATA_ALT, op_andi);
        table_fill(tb, (uint16_t)(0x0400 | s), 0x003F, EA_DATA_ALT, op_subi);
        table_fill(tb, (uint16_t)(0x0600 | s), 0x003F, EA_DATA_ALT, op_addi);
        table_fill(tb, (uint16_t)(0x0A00 | s), 0x003F, EA_DATA_ALT, op_eori);
        /* TODO(R3): CMPI takes PC-relative operands from the 68020 on. */
        table_fill(tb, (uint16_t)(0x0C00 | s), 0x003F, EA_DATA_ALT, op_cmpi);

        unsigned q = sz ? EA_ALTERABLE : EA_DATA_ALT;
        table_fill(tb, (uint16_t)(0x5000 | s), 0x0E3F, q, op_addq);
        table_fill(tb, (uint16_t)(0x5100 | s), 0x0E3F, q, op_subq);

        table_fill(tb, (uint16_t)(0x4000 | s), 0x003F, EA_DATA_ALT, op_negx);
        table_fill(tb, (uint16_t)(0x4400 | s), 0x003F, EA_DATA_ALT, op_neg);
        table_fill(tb, (uint16_t)(0x4600 | s), 0x003F, EA_DATA_ALT, op_not);
    }
    for (unsigned l = 0; l < 2; l++) {
        uint16_t line = l ? 0xD000 : 0x9000;
        relevo_handler h = l ? op_adda : op_suba;
        fill_line(tb, line, 3, EA_ALL, h);
        fill_line(tb, line, 7, EA_ALL, h);
    }
    fill_line(tb, 0xB000, 3, EA_ALL, op_cmpa);
    fill_line(tb, 0xB000, 7, EA_ALL, op_cmpa);
}
