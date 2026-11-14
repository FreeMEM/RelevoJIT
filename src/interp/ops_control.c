/* SPDX-License-Identifier: MPL-2.0
 *
 * Program control: the illegal/unimplemented opcodes, NOP, branches, jumps,
 * returns, TRAP, TRAPV and CHK.
 */
#include "interp.h"

void op_illegal(relevo_cpu *c, uint16_t op)
{
    (void)op;
    cpu_exception_group1(c, VEC_ILLEGAL);
}

/* Line A and line F: the host gets the first say (native calls), then the
 * exception, which stacks the address of the opcode itself. */
static void line_trap(relevo_cpu *c, uint16_t op,
                      relevo_trap_result (*fn)(void *, relevo_cpu *, uint16_t),
                      unsigned vector)
{
    if (fn != NULL) {
        switch (fn(c->host.ctx, c, op)) {
        case RELEVO_TRAP_HANDLED:
            return;
        case RELEVO_TRAP_STOP:
            c->exit = RELEVO_HOST_STOP;
            c->exiting = true;
            return;
        default:
            break;
        }
    }
    cpu_exception_group1(c, vector);
}

void op_line_a(relevo_cpu *c, uint16_t op)
{
    line_trap(c, op, c->host.line_a, VEC_LINE_A);
}

void op_line_f(relevo_cpu *c, uint16_t op)
{
    line_trap(c, op, c->host.line_f, VEC_LINE_F);
}

static void op_nop(relevo_cpu *c, uint16_t op)
{
    (void)c;
    (void)op;
}

/* Bcc, BRA, BSR: 8-bit displacement in the opcode, or 16 bits in the
 * extension word when it is 0 (32 bits when it is 0xFF, 68020 on). */
static uint32_t branch_target(relevo_cpu *c, uint16_t op)
{
    uint32_t base = c->insn_pc + 2;
    uint32_t disp = op & 0xFF;
    if (disp == 0)
        return base + sext(fetch16(c), SZ_W);
    if (disp == 0xFF && c->model >= RELEVO_68020)
        return base + fetch32(c);
    return base + sext(disp, SZ_B);
}

static void op_bcc(relevo_cpu *c, uint16_t op)
{
    if (cond_true(c->r.sr, (op >> 8) & 15)) {
        uint32_t target = branch_target(c, op);
        cycles(c, 2);
        jump(c, target, c->insn_pc + 2);
        return;
    }
    cycles(c, 4);
    if ((op & 0xFF) == 0) {
        c->r.pc += 2;
        cycles(c, 4);       /* the skipped word is still prefetched */
    } else if ((op & 0xFF) == 0xFF && c->model >= RELEVO_68020) {
        c->r.pc += 4;
        cycles(c, 8);
    }
}

static void op_bsr(relevo_cpu *c, uint16_t op)
{
    uint32_t target = branch_target(c, op);
    cycles(c, 2);
    cpu_push32(c, c->r.pc);
    jump(c, target, target);
}

/* DBcc Dn,<disp>: the counter is written only once the branch is safe. */
static void op_dbcc(relevo_cpu *c, uint16_t op)
{
    uint32_t target = c->insn_pc + 2 + sext(fetch16(c), SZ_W);
    if (cond_true(c->r.sr, (op >> 8) & 15)) {
        cycles(c, 4);
        return;
    }
    unsigned n = op & 7;
    uint32_t count = (c->r.d[n] - 1) & 0xFFFF;
    if (count != 0xFFFF) {
        cycles(c, 2);
        jump(c, target, c->r.pc);
    } else {
        cycles(c, 6);
    }
    set_dreg(c, n, count, SZ_W);
}

/* Scc <ea>: the 68000 reads the byte before writing it. */
static void op_scc(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_B);
    ea_read(c, &e);
    bool t = cond_true(c->r.sr, (op >> 8) & 15);
    if (t && e.kind == EA_DREG)
        cycles(c, 2);
    ea_write(c, &e, t ? 0xFF : 0);
}

/* JMP and JSR take 2 cycles more for (d16,An), (d16,PC) and (xxx).W, and
 * 2 more again for the indexed modes. */
static void jump_ea_cycles(relevo_cpu *c, const ea_t *e)
{
    if (e->mode == 5 || (e->mode == 7 && (e->reg == 0 || e->reg == 2)))
        cycles(c, 2);
    else if (e->mode == 6 || (e->mode == 7 && e->reg == 3))
        cycles(c, 4);
}

static void op_jmp(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    jump_ea_cycles(c, &e);
    jump(c, e.addr, c->insn_pc + 2);
}

/* JSR: the return address goes on the stack only after the target is
 * known to be good. */
static void op_jsr(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_L);
    jump_ea_cycles(c, &e);
    uint32_t ret = c->r.pc;
    if (e.addr & 1)
        jump(c, e.addr, ret);
    cpu_push32(c, ret);
    jump(c, e.addr, ret);
}

static void op_rts(relevo_cpu *c, uint16_t op)
{
    (void)op;
    uint32_t target = cpu_pop32(c);
    jump(c, target, c->insn_pc + 2);
}

static void op_rtr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    uint32_t ccr = cpu_pop16(c);
    uint32_t target = cpu_pop32(c);
    set_ccr_bits(c, 0x1F, ccr);
    jump(c, target, c->insn_pc + 2);
}

/* RTE, 68000 frame (SR, PC). TODO(R3): the 68010+ frame formats. */
static void op_rte(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (!supervisor_or_trap(c))
        return;
    uint32_t sr = cpu_pop16(c);
    uint32_t target = cpu_pop32(c);
    cpu_set_sr(c, (uint16_t)sr);
    jump(c, target, c->insn_pc + 2);
}

static void op_trap(relevo_cpu *c, uint16_t op)
{
    cpu_exception(c, VEC_TRAP + (op & 15), c->r.pc);
}

static void op_trapv(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (c->r.sr & CCR_V)
        cpu_exception(c, VEC_TRAPV, c->r.pc);
}

/* CHK.W <ea>,Dn: trap if Dn < 0 (N set) or Dn > bound (N clear). */
static void op_chk(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    int32_t bound = (int32_t)sext(ea_read(c, &e), SZ_W);
    int32_t v = (int32_t)sext(c->r.d[(op >> 9) & 7], SZ_W);
    unsigned f = 0;
    if (v == 0)
        f |= CCR_Z;
    if (v < 0) {
        set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f | CCR_N);
        /* 2 cycles more unless Dn - bound, in 16 bits, looks positive */
        uint32_t diff = (uint32_t)(v - bound) & 0xFFFF;
        cycles(c, diff != 0 && !(diff & 0x8000) ? 4 : 6);
        cpu_exception(c, VEC_CHK, c->r.pc);
    } else if (v > bound) {
        set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
        cycles(c, 4);
        cpu_exception(c, VEC_CHK, c->r.pc);
    } else {
        set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
        cycles(c, 6);
    }
}

void ops_control_init(op_table *tb)
{
    table_set(tb, 0x4E71, op_nop);
    table_set(tb, 0x4E73, op_rte);
    table_set(tb, 0x4E75, op_rts);
    table_set(tb, 0x4E76, op_trapv);
    table_set(tb, 0x4E77, op_rtr);
    table_fill(tb, 0x4E40, 0x000F, 0, op_trap);
    table_fill(tb, 0x4EC0, 0x003F, EA_CONTROL, op_jmp);
    table_fill(tb, 0x4E80, 0x003F, EA_CONTROL, op_jsr);
    for (uint16_t cc = 0; cc < 16; cc++)                    /* BRA, Bcc */
        if (cc != 1)
            table_fill(tb, (uint16_t)(0x6000 | cc << 8), 0x00FF, 0, op_bcc);
    table_fill(tb, 0x6100, 0x00FF, 0, op_bsr);
    table_fill(tb, 0x50C8, 0x0F07, 0, op_dbcc);
    table_fill(tb, 0x50C0, 0x0F3F, EA_DATA_ALT, op_scc);
    table_fill(tb, 0x4180, 0x0E3F, EA_DATA, op_chk);
}
