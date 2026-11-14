/* SPDX-License-Identifier: MPL-2.0
 *
 * What the 68010 added, also on the later models: MOVEC, MOVES, RTD and
 * MOVE from CCR. (MOVE from SR becoming privileged is in ops_system.c; the
 * new exception frames and RTE are in exception.c and ops_control.c.)
 */
#include "../cache.h"
#include "interp.h"

/* MOVEC control register codes (MC68020UM, MC68040UM, MC68060UM). */
enum {
    CR_SFC = 0x000, CR_DFC = 0x001, CR_CACR = 0x002, CR_TC = 0x003,
    CR_ITT0 = 0x004, CR_ITT1 = 0x005, CR_DTT0 = 0x006, CR_DTT1 = 0x007,
    CR_BUSCR = 0x008,
    CR_USP = 0x800, CR_VBR = 0x801, CR_CAAR = 0x802, CR_MSP = 0x803,
    CR_ISP = 0x804, CR_MMUSR = 0x805, CR_URP = 0x806, CR_SRP = 0x807,
    CR_PCR = 0x808
};

/* Whether the model has control register `cr`. */
static bool creg_exists(relevo_model m, unsigned cr)
{
    switch (cr) {
    case CR_SFC: case CR_DFC: case CR_USP: case CR_VBR:
        return true;
    case CR_CACR:
        return m >= RELEVO_68020;
    case CR_CAAR:
        return m == RELEVO_68020 || m == RELEVO_68030;
    case CR_MSP: case CR_ISP:
        return m >= RELEVO_68020 && m <= RELEVO_68040;
    case CR_TC: case CR_ITT0: case CR_ITT1: case CR_DTT0: case CR_DTT1:
    case CR_URP: case CR_SRP:
        return m >= RELEVO_68040;
    case CR_MMUSR:
        return m == RELEVO_68040;
    case CR_BUSCR: case CR_PCR:
        return m == RELEVO_68060;
    default:
        return false;
    }
}

/* Bits that hold a value (MC68040UM 3.1 and 4.1; MC68060UM 4.1, 6.1). */
#define TC_MASK     0x0000C000u     /* E, P */
#define TT_MASK     0xFFFFE364u     /* base, mask, E, S, U1-U0, CM, W */
#define RP_MASK     0xFFFFFE00u     /* 512-byte aligned table */
#define BUSCR_MASK  0xF0000000u
#define PCR_MASK    0x00000083u     /* EDEBUG, DFP, ESS */
#define PCR_ID      0x04300000u     /* 68060, revision 0 */

/* The stack pointer that M and S select, wherever it lives now: a[7] when
 * it is the active one. */
static uint32_t *stack_reg(relevo_cpu *c, unsigned cr)
{
    bool s = c->r.sr & RELEVO_SR_S, m = c->r.sr & RELEVO_SR_M;
    switch (cr) {
    case CR_USP:
        return s ? &c->r.usp : &c->r.a[7];
    case CR_MSP:
        return s && m ? &c->r.a[7] : &c->r.msp;
    default:    /* ISP */
        return s && !m ? &c->r.a[7] : &c->r.isp;
    }
}

static uint32_t creg_read(relevo_cpu *c, unsigned cr)
{
    switch (cr) {
    case CR_SFC:   return c->r.sfc;
    case CR_DFC:   return c->r.dfc;
    case CR_CACR:  return c->r.cacr;
    case CR_TC:    return c->r.tc;
    case CR_ITT0:  return c->r.itt0;
    case CR_ITT1:  return c->r.itt1;
    case CR_DTT0:  return c->r.dtt0;
    case CR_DTT1:  return c->r.dtt1;
    case CR_BUSCR: return c->r.buscr;
    case CR_VBR:   return c->r.vbr;
    case CR_CAAR:  return c->r.caar;
    case CR_MMUSR: return c->r.mmusr;
    case CR_URP:   return c->r.urp;
    case CR_SRP:   return c->r.srp;
    case CR_PCR:   return PCR_ID | c->r.pcr;
    default:       return *stack_reg(c, cr);
    }
}

static void creg_write(relevo_cpu *c, unsigned cr, uint32_t v)
{
    switch (cr) {
    case CR_SFC:   c->r.sfc = v & 7; break;
    case CR_DFC:   c->r.dfc = v & 7; break;
    case CR_CACR:  cache_write_cacr(c, v); break;
    case CR_TC:    c->r.tc = v & TC_MASK; break;
    case CR_ITT0:  c->r.itt0 = v & TT_MASK; break;
    case CR_ITT1:  c->r.itt1 = v & TT_MASK; break;
    case CR_DTT0:  c->r.dtt0 = v & TT_MASK; break;
    case CR_DTT1:  c->r.dtt1 = v & TT_MASK; break;
    case CR_BUSCR: c->r.buscr = v & BUSCR_MASK; break;
    case CR_VBR:   c->r.vbr = v; break;
    case CR_CAAR:  c->r.caar = v; break;
    case CR_MMUSR: c->r.mmusr = v; break;     /* every bit, as FS-UAE */
    case CR_URP:   c->r.urp = v & RP_MASK; break;
    case CR_SRP:   c->r.srp = v & RP_MASK; break;
    case CR_PCR:   c->r.pcr = v & PCR_MASK; break;
    default:       *stack_reg(c, cr) = v; break;
    }
}

/* MOVEC Rc,Rn (0x4E7A) and MOVEC Rn,Rc (0x4E7B). A register the model
 * lacks is an illegal instruction. */
static void op_movec(relevo_cpu *c, uint16_t op)
{
    if (!supervisor_or_trap(c))
        return;
    uint32_t ext = fetch16(c);
    unsigned cr = ext & 0x0FFF;
    if (!creg_exists(c->model, cr)) {
        cpu_exception_group1(c, VEC_ILLEGAL);
        return;
    }
    uint32_t *rn = (ext & 0x8000) ? &c->r.a[(ext >> 12) & 7]
                                  : &c->r.d[(ext >> 12) & 7];
    if (op & 1)
        creg_write(c, cr, *rn);
    else
        *rn = creg_read(c, cr);
    cycles(c, 6);
}

/* MOVES <ea>,Rn and MOVES Rn,<ea>: an access with the function code in SFC
 * or DFC. RelevoJIT has no function codes on its bus, so it is an ordinary
 * access. Into An, the operand is sign-extended. */
static void op_moves(relevo_cpu *c, uint16_t op)
{
    if (!supervisor_or_trap(c))
        return;
    uint32_t ext = fetch16(c);
    unsigned size = size_field(op);
    unsigned n = (ext >> 12) & 7;
    bool areg = ext & 0x8000;
    ea_t e;
    if (ext & 0x0800) {
        uint32_t v = areg ? c->r.a[n] : c->r.d[n];
        ea_decode_op(c, &e, op, size);
        ea_write(c, &e, v);
    } else {
        ea_decode_op(c, &e, op, size);
        uint32_t v = ea_read(c, &e);
        if (areg)
            c->r.a[n] = sext(v, size);
        else
            set_dreg(c, n, v, size);
    }
    cycles(c, 4);
}

/* RTD #d16: return and deallocate. */
static void op_rtd(relevo_cpu *c, uint16_t op)
{
    (void)op;
    uint32_t disp = sext(fetch16(c), SZ_W);
    uint32_t target = cpu_pop32(c);
    c->r.a[7] += disp;
    jump(c, target, c->insn_pc + 2);
}

/* MOVE CCR,<ea>: a word, the CCR in the low byte. */
static void op_move_from_ccr(relevo_cpu *c, uint16_t op)
{
    ea_t e;
    ea_decode_op(c, &e, op, SZ_W);
    ea_write(c, &e, c->r.sr & 0x1F);
}

void ops_010_init(op_table *tb)
{
    if (tb->model < RELEVO_68010)
        return;
    table_set(tb, 0x4E7A, op_movec);
    table_set(tb, 0x4E7B, op_movec);
    table_set(tb, 0x4E74, op_rtd);
    table_fill(tb, 0x42C0, 0x003F, EA_DATA_ALT, op_move_from_ccr);
    for (uint16_t s = 0; s < 3; s++)
        table_fill(tb, (uint16_t)(0x0E00 | s << 6), 0x003F, EA_MEM_ALT,
                   op_moves);
}
