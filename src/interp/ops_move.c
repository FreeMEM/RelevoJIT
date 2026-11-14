/* SPDX-License-Identifier: MPL-2.0
 *
 * Data movement: MOVE, MOVEA, MOVEQ, MOVEM, MOVEP, LEA, PEA, EXG, SWAP,
 * LINK, UNLK, CLR, TST, EXT and TAS.
 */
#include "interp.h"

/* ---- MOVE and MOVEA --------------------------------------------------- */

/* MOVE.L writes its destination before its flags are complete. If that
 * write faults, the 68000 has stacked an SR whose N and Z depend on how far
 * it got (SingleStepTests data): */
static void move_long_fault_flags(relevo_cpu *c, const ea_t *src,
                                  const ea_t *dst, uint32_t v)
{
    bool abs_l = dst->mode == 7 && dst->reg == 1;
    bool ind = dst->mode == 2 || dst->mode == 3;
    if (src->kind == EA_MEM) {
        /* the low word read last decides */
        if (ind || abs_l)
            set_nz(c, v & 0xFFFF, SZ_W);
        else
            set_nz(c, v, SZ_L);
    } else if (ind) {
        /* nothing set yet */
    } else if (dst->mode == 5 || dst->mode == 6) {
        /* N and Z only */
        unsigned f = (v == 0 ? CCR_Z : 0) | (v & 0x80000000u ? CCR_N : 0);
        set_ccr_bits(c, CCR_N | CCR_Z, f);
    } else {
        set_nz(c, v, SZ_L);
    }
}

static void move(relevo_cpu *c, uint16_t op, unsigned size)
{
    ea_t s, d;
    ea_decode_op(c, &s, op, size);
    uint32_t v = ea_read(c, &s);
    uint32_t pc = c->r.pc;
    ea_decode(c, &d, (op >> 6) & 7, (op >> 9) & 7, size);
    if (d.kind == EA_MEM) {
        /* The next prefetch happens before the write: the stacked PC is
         * one word further on, two for (xxx).L after a source that is not
         * in memory. */
        bool abs_l = d.mode == 7 && d.reg == 1;
        d.ae_pc = pc + 2 + (abs_l && s.kind != EA_MEM ? 2 : 0);
        /* If the write faults, the 68000's prefetches are one word ahead
         * of the words counted here for -(An), one behind for (xxx).L
         * after a memory source. */
        if (d.mode == 4)
            c->fault_cycles = 4;
        else if (abs_l && s.kind == EA_MEM)
            c->fault_cycles = -4;
        if (size == SZ_L) {
            move_long_fault_flags(c, &s, &d, v);
        } else {
            set_nz(c, v, size);
            /* MOVE.W to -(An) has already loaded the next opcode. */
            if (d.mode == 4)
                c->ir = (uint16_t)mem_read16(c, c->r.pc);
        }
    }
    ea_write(c, &d, v);
    c->ir = op;
    set_nz(c, v, size);
}

static void op_move_b(relevo_cpu *c, uint16_t op) { move(c, op, SZ_B); }
static void op_move_w(relevo_cpu *c, uint16_t op) { move(c, op, SZ_W); }
static void op_move_l(relevo_cpu *c, uint16_t op) { move(c, op, SZ_L); }

static void op_movea_w(relevo_cpu *c, uint16_t op)
{
    ea_t s;
    ea_decode_op(c, &s, op, SZ_W);
    c->r.a[(op >> 9) & 7] = sext(ea_read(c, &s), SZ_W);
}

static void op_movea_l(relevo_cpu *c, uint16_t op)
{
    ea_t s;
    ea_decode_op(c, &s, op, SZ_L);
    c->r.a[(op >> 9) & 7] = ea_read(c, &s);
}

static void op_moveq(relevo_cpu *c, uint16_t op)
{
    uint32_t v = sext(op, SZ_B);
    c->r.d[(op >> 9) & 7] = v;
    set_nz(c, v, SZ_L);
}

/* ---- MOVEM ------------------------------------------------------------ */

/* Registers by number: 0-7 D0-D7, 8-15 A0-A7. */
static uint32_t *reg_n(relevo_cpu *c, unsigned n)
{
    return n < 8 ? &c->r.d[n] : &c->r.a[n - 8];
}

/* Registers to memory. The mask is read first, then the address's
 * extension words; the transfers come after the next prefetch. */
static void movem_to_mem(relevo_cpu *c, uint16_t op, unsigned size)
{
    unsigned mask = fetch16(c);
    unsigned mode = (op >> 3) & 7, an = op & 7;
    if (mode == 4) {
        /* -(An): A7 down to D0 (mask bit 0 is A7); An itself is stored
         * with its value before the instruction. */
        uint32_t addr = c->r.a[an];
        c->ae_pc = c->r.pc + 2;
        for (unsigned i = 0; i < 16; i++) {
            if (!(mask & (1u << i)))
                continue;
            uint32_t v = *reg_n(c, 15 - i);
            addr -= size;
            if (size == SZ_L) {
                cpu_write16(c, addr + 2, v & 0xFFFF);
                cpu_write16(c, addr, v >> 16);
            } else {
                cpu_write16(c, addr, v);
            }
        }
        c->r.a[an] = addr;
        return;
    }
    ea_t e;
    ea_decode(c, &e, mode, an, size);
    uint32_t addr = e.addr;
    c->ae_pc = c->r.pc + 2;
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i)))
            continue;
        cpu_write(c, addr, *reg_n(c, i), size);
        addr += size;
    }
}

/* Memory to registers, D0 to A7; words are sign-extended to 32 bits. With
 * (An)+, An ends at the address after the last one, even if it was in the
 * list. */
static void movem_to_regs(relevo_cpu *c, uint16_t op, unsigned size)
{
    unsigned mask = fetch16(c);
    ea_t e;
    ea_decode_op(c, &e, op, size);
    uint32_t addr = e.addr;
    c->ae_pc = c->r.pc + 2;
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i)))
            continue;
        uint32_t v = cpu_read(c, addr, size, e.acc);
        *reg_n(c, i) = size == SZ_W ? sext(v, SZ_W) : v;
        addr += size;
    }
    cycles(c, 4);   /* the 68000 reads one word more */
    if (e.mode == 3)
        c->r.a[e.reg] = addr;
}

static void op_movem_w_to_mem(relevo_cpu *c, uint16_t op)
{
    movem_to_mem(c, op, SZ_W);
}

static void op_movem_l_to_mem(relevo_cpu *c, uint16_t op)
{
    movem_to_mem(c, op, SZ_L);
}

static void op_movem_w_to_regs(relevo_cpu *c, uint16_t op)
{
    movem_to_regs(c, op, SZ_W);
}

static void op_movem_l_to_regs(relevo_cpu *c, uint16_t op)
{
    movem_to_regs(c, op, SZ_L);
}

/* ---- MOVEP: bytes at every other address ------------------------------ */

static void op_movep(relevo_cpu *c, uint16_t op)
{
    unsigned dn = (op >> 9) & 7;
    uint32_t addr = c->r.a[op & 7] + sext(fetch16(c), SZ_W);
    unsigned bytes = (op & 0x40) ? 4 : 2;
    if (op & 0x80) {
        uint32_t v = c->r.d[dn];
        for (unsigned i = 0; i < bytes; i++)
            cpu_write8(c, addr + 2 * i, v >> (8 * (bytes - 1 - i)));
    } else {
        uint32_t v = 0;
        for (unsigned i = 0; i < bytes; i++)
            v = v << 8 | cpu_read8(c, addr + 2 * i, ACC_DATA);
        set_dreg(c, dn, v, bytes);
    }
}

/* ---- Addresses -------------------------------------------------------- */

static void op_lea(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    if (e.mode == 6 || (e.mode == 7 && e.reg == 3))
        cycles(c, 2);
    c->r.a[(op >> 9) & 7] = e.addr;
}

static void op_pea(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    if (e.mode == 6 || (e.mode == 7 && e.reg == 3))
        cycles(c, 2);
    cpu_push32(c, e.addr);
}

/* LINK An,#d16: push An, An = SP, SP += d16. */
static void op_link(relevo_cpu *c, uint16_t op)
{
    unsigned n = op & 7;
    uint32_t d = sext(fetch16(c), SZ_W);
    cpu_push32(c, c->r.a[n]);  /* LINK A7 stores SP before the push */
    c->r.a[n] = c->r.a[7];
    c->r.a[7] += d;
}

/* UNLK An: SP = An, An = pop. */
static void op_unlk(relevo_cpu *c, uint16_t op)
{
    unsigned n = op & 7;
    uint32_t addr = c->r.a[n];
    c->ae_pc = c->r.pc + 2;
    uint32_t v = cpu_read32(c, addr, ACC_DATA);
    c->r.a[7] = addr + 4;
    c->r.a[n] = v;
}

/* ---- Registers -------------------------------------------------------- */

static void op_exg(relevo_cpu *c, uint16_t op)
{
    unsigned x = (op >> 9) & 7, y = op & 7;
    uint32_t *rx, *ry;
    switch ((op >> 3) & 0x1F) {
    case 0x08: rx = &c->r.d[x]; ry = &c->r.d[y]; break;
    case 0x09: rx = &c->r.a[x]; ry = &c->r.a[y]; break;
    default:   rx = &c->r.d[x]; ry = &c->r.a[y]; break;
    }
    uint32_t t = *rx;
    *rx = *ry;
    *ry = t;
    cycles(c, 2);
}

static void op_swap(relevo_cpu *c, uint16_t op)
{
    uint32_t v = c->r.d[op & 7];
    v = v << 16 | v >> 16;
    c->r.d[op & 7] = v;
    set_nz(c, v, SZ_L);
}

static void op_ext_w(relevo_cpu *c, uint16_t op)
{
    uint32_t v = sext(c->r.d[op & 7], SZ_B);
    set_dreg(c, op & 7, v, SZ_W);
    set_nz(c, v, SZ_W);
}

static void op_ext_l(relevo_cpu *c, uint16_t op)
{
    uint32_t v = sext(c->r.d[op & 7], SZ_W);
    c->r.d[op & 7] = v;
    set_nz(c, v, SZ_L);
}

/* ---- CLR, TST, TAS ---------------------------------------------------- */

/* CLR: the 68000 reads the operand before clearing it. */
static void op_clr(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, size_field(op));
    ea_read(c, &e);
    if (e.kind == EA_DREG && e.size == SZ_L)
        cycles(c, 2);
    ea_write(c, &e, 0);
    set_nz(c, 0, SZ_B);
}

static void op_tst(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    unsigned size = size_field(op);
    ea_decode_op(c, &e, op, size);
    set_nz(c, ea_read(c, &e), size);
}

static void op_tas(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_B);
    uint32_t v = ea_read(c, &e);
    set_nz(c, v, SZ_B);
    if (e.kind == EA_MEM)
        cycles(c, 2);
    ea_write(c, &e, v | 0x80);
}

/* ---- Table ------------------------------------------------------------ */

/* MOVE: source in bits 5-0, destination register/mode in bits 11-6. */
static void fill_move(op_table *tb, uint16_t base, unsigned src_classes,
                      relevo_handler h, relevo_handler ha)
{
    for (unsigned dst = 0; dst < 64; dst++) {
        unsigned dmode = dst & 7, dreg = dst >> 3;
        relevo_handler fn;
        if (ea_valid(dmode, dreg, EA_DATA_ALT))
            fn = h;
        else if (dmode == 1 && ha != NULL)
            fn = ha;
        else
            continue;
        table_fill(tb, (uint16_t)(base | dst << 6), 0x003F, src_classes, fn);
    }
}

void ops_move_init(op_table *tb)
{
    fill_move(tb, 0x1000, EA_DATA, op_move_b, NULL);
    fill_move(tb, 0x3000, EA_ALL, op_move_w, op_movea_w);
    fill_move(tb, 0x2000, EA_ALL, op_move_l, op_movea_l);
    table_fill(tb, 0x7000, 0x0EFF, 0, op_moveq);

    table_fill(tb, 0x4880, 0x003F, EA_CONTROL_ALT | EAB_PREDEC,
               op_movem_w_to_mem);
    table_fill(tb, 0x48C0, 0x003F, EA_CONTROL_ALT | EAB_PREDEC,
               op_movem_l_to_mem);
    table_fill(tb, 0x4C80, 0x003F, EA_CONTROL | EAB_POSTINC,
               op_movem_w_to_regs);
    table_fill(tb, 0x4CC0, 0x003F, EA_CONTROL | EAB_POSTINC,
               op_movem_l_to_regs);
    table_fill(tb, 0x0108, 0x0EC7, 0, op_movep);

    table_fill(tb, 0x41C0, 0x0E3F, EA_CONTROL, op_lea);
    table_fill(tb, 0x4840, 0x003F, EA_CONTROL, op_pea);
    table_fill(tb, 0x4E50, 0x0007, 0, op_link);
    table_fill(tb, 0x4E58, 0x0007, 0, op_unlk);

    table_fill(tb, 0xC140, 0x0E07, 0, op_exg);
    table_fill(tb, 0xC148, 0x0E07, 0, op_exg);
    table_fill(tb, 0xC188, 0x0E07, 0, op_exg);
    table_fill(tb, 0x4840, 0x0007, 0, op_swap);
    table_fill(tb, 0x4880, 0x0007, 0, op_ext_w);
    table_fill(tb, 0x48C0, 0x0007, 0, op_ext_l);

    for (uint16_t size = 0; size < 3; size++) {
        table_fill(tb, (uint16_t)(0x4200 | size << 6), 0x003F, EA_DATA_ALT,
                   op_clr);
        table_fill(tb, (uint16_t)(0x4A00 | size << 6), 0x003F, EA_DATA_ALT,
                   op_tst);
    }
    table_fill(tb, 0x4AC0, 0x003F, EA_DATA_ALT, op_tas);
}
