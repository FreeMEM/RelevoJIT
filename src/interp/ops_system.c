/* SPDX-License-Identifier: MPL-2.0
 *
 * System control: the status register and CCR, USP, STOP and RESET.
 */
#include "interp.h"

/* MOVE from SR: not privileged on the 68000, where it reads the operand
 * first, like CLR; privileged from the 68010 on. */
static void op_move_from_sr(relevo_cpu *c, uint16_t op)
{
    if (c->model != RELEVO_68000) {
        if (!supervisor_or_trap(c))
            return;
        ea_t e;
        ea_decode_op(c, &e, op, SZ_W);
        ea_write(c, &e, c->r.sr);
        return;
    }
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    ea_read(c, &e);
    if (e.kind == EA_DREG)
        cycles(c, 2);
    ea_write(c, &e, c->r.sr);
}

static void op_move_to_ccr(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    set_ccr_bits(c, 0x1F, ea_read(c, &e));
    cycles(c, 8);
}

static void op_move_to_sr(relevo_cpu *c, uint16_t op)
{
    if (!supervisor_or_trap(c))
        return;
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    cpu_set_sr(c, (uint16_t)ea_read(c, &e));
    cycles(c, 8);
}

/* MOVE An,USP and MOVE USP,An. a[7] is the SSP here (supervisor mode). */
static void op_move_usp(relevo_cpu *c, uint16_t op)
{
    if (!supervisor_or_trap(c))
        return;
    if (op & 8)
        c->r.a[op & 7] = c->r.usp;
    else
        c->r.usp = c->r.a[op & 7];
}

/* ANDI, ORI and EORI to CCR and SR: 20 cycles, 12 of them internal (the
 * 68000 re-reads the next word, too). */
static void op_ori_ccr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    c->r.sr |= (uint16_t)(fetch16(c) & 0x1F);
    cycles(c, 12);
}

static void op_andi_ccr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    c->r.sr &= (uint16_t)(fetch16(c) | 0xFF00);
    cycles(c, 12);
}

static void op_eori_ccr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    c->r.sr ^= (uint16_t)(fetch16(c) & 0x1F);
    cycles(c, 12);
}

static void op_ori_sr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (supervisor_or_trap(c)) {
        cpu_set_sr(c, (uint16_t)(c->r.sr | fetch16(c)));
        cycles(c, 12);
    }
}

static void op_andi_sr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (supervisor_or_trap(c)) {
        cpu_set_sr(c, (uint16_t)(c->r.sr & fetch16(c)));
        cycles(c, 12);
    }
}

static void op_eori_sr(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (supervisor_or_trap(c)) {
        cpu_set_sr(c, (uint16_t)(c->r.sr ^ fetch16(c)));
        cycles(c, 12);
    }
}

/* STOP #imm: loads SR and waits for an interrupt (or a trace). */
static void op_stop(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (!supervisor_or_trap(c))
        return;
    cpu_set_sr(c, (uint16_t)fetch16(c));
    c->cycles -= 4;     /* the operand was in the queue: no refill */
    c->stopped = true;
    c->exit = RELEVO_STOPPED;
    c->exiting = true;
}

/* RESET: resets the devices, not the CPU. */
static void op_reset(relevo_cpu *c, uint16_t op)
{
    (void)op;
    if (!supervisor_or_trap(c))
        return;
    cycles(c, 128);
    if (c->host.reset != NULL)
        c->host.reset(c->host.ctx);
}

void ops_system_init(op_table *tb)
{
    table_fill(tb, 0x40C0, 0x003F, EA_DATA_ALT, op_move_from_sr);
    table_fill(tb, 0x44C0, 0x003F, EA_DATA, op_move_to_ccr);
    table_fill(tb, 0x46C0, 0x003F, EA_DATA, op_move_to_sr);
    table_fill(tb, 0x4E60, 0x000F, 0, op_move_usp);
    table_set(tb, 0x003C, op_ori_ccr);
    table_set(tb, 0x007C, op_ori_sr);
    table_set(tb, 0x023C, op_andi_ccr);
    table_set(tb, 0x027C, op_andi_sr);
    table_set(tb, 0x0A3C, op_eori_ccr);
    table_set(tb, 0x0A7C, op_eori_sr);
    table_set(tb, 0x4E72, op_stop);
    table_set(tb, 0x4E70, op_reset);
}
