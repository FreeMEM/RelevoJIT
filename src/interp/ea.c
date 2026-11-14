/* SPDX-License-Identifier: MPL-2.0
 *
 * Effective addresses of the 68000 (M68000PRM chapter 2) and of the 68020
 * (scaled index, full extension word, memory indirect: MC68020UM 2.2),
 * condition codes and jumps.
 *
 * What the 68000 does when an access faults is not in the manuals; the
 * rules here come from the SingleStepTests/m68000 data:
 *
 * - (An)+ on a read: the register moves before the access for bytes and
 *   words, after it for longs. -(An) on a read: before the access.
 *   On a write that is not read first (MOVE): (An)+ after the access;
 *   -(An) before it for bytes and words, after it for longs. A long write
 *   to -(An) writes the low word first, at the higher address.
 * - The PC stacked when a read faults is the PC as it was before the
 *   operand's extension words, except for (xxx).W and (xxx).L (after
 *   them) and for -(An) on bytes and words (2 more). Writes that come after
 *   the next prefetch (MOVE, MOVEM) set their own value in ea.ae_pc.
 */
#include "interp.h"

bool cond_true(uint16_t sr, unsigned cc)
{
    bool c = sr & CCR_C, v = sr & CCR_V, z = sr & CCR_Z, n = sr & CCR_N;
    switch (cc & 15) {
    case 0:  return true;
    case 1:  return false;
    case 2:  return !c && !z;           /* HI */
    case 3:  return c || z;             /* LS */
    case 4:  return !c;                 /* CC */
    case 5:  return c;                  /* CS */
    case 6:  return !z;                 /* NE */
    case 7:  return z;                  /* EQ */
    case 8:  return !v;                 /* VC */
    case 9:  return v;                  /* VS */
    case 10: return !n;                 /* PL */
    case 11: return n;                  /* MI */
    case 12: return n == v;             /* GE */
    case 13: return n != v;             /* LT */
    case 14: return !z && n == v;       /* GT */
    default: return z || n != v;        /* LE */
    }
}

void jump(relevo_cpu *c, uint32_t target, uint32_t ae_pc)
{
    /* The queue gets two words from the target. The instruction's words,
     * counted when fetched, stand for the 68000's reads, except that it
     * never refills behind the last extension word of a jump. */
    uint32_t words = (c->r.pc - c->insn_pc) / 2;
    if (UNLIKELY(target & 1)) {
        if (words >= 2)
            c->cycles -= 4;
        c->ae_pc = ae_pc;
        cpu_address_error(c, target, ACC_PROG | ACC_READ);
    }
    if (words < 2)
        cycles(c, 8 - 4 * words);
    c->r.pc = target;
}

/* An encoding the CPU does not accept, found while decoding an operand:
 * the illegal instruction exception, and the instruction ends here. */
static NORETURN void ea_illegal(relevo_cpu *c)
{
    cpu_exception_group1(c, VEC_ILLEGAL);
    longjmp(c->fault_jmp, 1);
}

/* The index register of an extension word: Dn or An, its low word sign
 * extended or all of it, times the scale (68020 on). */
static uint32_t index_value(relevo_cpu *c, uint32_t ext, bool scaled)
{
    unsigned xn = (ext >> 12) & 7;
    uint32_t x = (ext & 0x8000) ? c->r.a[xn] : c->r.d[xn];
    if (!(ext & 0x0800))
        x = sext(x, SZ_W);
    if (scaled)
        x <<= (ext >> 9) & 3;
    return x;
}

/* The 68020's full extension word (MC68020UM 2.2): base and index
 * suppress (bits 7 and 6), base displacement size (bits 5-4: 01 none, 10
 * word, 11 long) and the memory indirection (bits 2-0, I/IS):
 *
 *   index used:      000 none, 001-011 pre-indexed, 101-111 post-indexed
 *   index suppressed: 000 none, 001-011 memory indirect
 *
 * with an outer displacement of none, a word or a long (low 2 bits 01, 10,
 * 11). The displacements follow the extension word, base first. The
 * indirect address is a long read from data space. The reserved encodings
 * (displacement size 00, I/IS 100, or 1xx with the index suppressed) take
 * the illegal instruction exception. */
static uint32_t full_format(relevo_cpu *c, uint32_t base, uint32_t ext)
{
    unsigned iis = ext & 7;
    bool is = (ext & 0x0040) != 0;
    if ((ext & 0x0030) == 0 || iis == 4 || (is && iis > 4))
        ea_illegal(c);
    uint32_t bd = 0;
    if (((ext >> 4) & 3) == 2)
        bd = sext(fetch16(c), SZ_W);
    else if (((ext >> 4) & 3) == 3)
        bd = fetch32(c);
    uint32_t od = 0;
    if ((iis & 3) == 2)
        od = sext(fetch16(c), SZ_W);
    else if ((iis & 3) == 3)
        od = fetch32(c);
    if (ext & 0x0080)
        base = 0;
    uint32_t x = is ? 0 : index_value(c, ext, true);
    if (iis == 0)
        return base + bd + x;
    if (iis & 4)    /* post-indexed */
        return cpu_read32(c, base + bd, ACC_DATA) + x + od;
    return cpu_read32(c, base + bd + x, ACC_DATA) + od;
}

/* (d8,An,Xn) and (d8,PC,Xn): the brief extension word. The 68000 and
 * 68010 ignore bits 10-8; from the 68020 on, bits 10-9 scale the index and
 * bit 8 selects the full format. The addition takes 2 cycles more. */
static uint32_t indexed(relevo_cpu *c, uint32_t base)
{
    uint32_t ext = fetch16(c);
    cycles(c, 2);
    if (c->model < RELEVO_68020)
        return base + sext(ext, SZ_B) + index_value(c, ext, false);
    if (ext & 0x0100)
        return full_format(c, base, ext);
    return base + sext(ext, SZ_B) + index_value(c, ext, true);
}

void ea_decode(relevo_cpu *c, ea_t *e, unsigned mode, unsigned reg,
               unsigned size)
{
    uint32_t pc0 = c->r.pc;
    e->mode = (uint8_t)mode;
    e->reg = (uint8_t)reg;
    e->size = (uint8_t)size;
    e->step = 0;
    e->acc = ACC_DATA;
    e->ae_pc = pc0;
    e->kind = EA_MEM;

    /* (A7)+ and -(A7) keep the stack word aligned for bytes. */
    int8_t step = (int8_t)(size == SZ_B && reg == 7 ? 2 : size);

    switch (mode) {
    case 0:
        e->kind = EA_DREG;
        e->addr = reg;
        return;
    case 1:
        e->kind = EA_AREG;
        e->addr = reg;
        return;
    case 2:
        e->addr = c->r.a[reg];
        return;
    case 3:
        e->addr = c->r.a[reg];
        e->step = step;
        return;
    case 4:
        e->addr = c->r.a[reg] - (uint32_t)step;
        e->step = (int8_t)-step;
        if (size != SZ_L)
            e->ae_pc = pc0 + 2;
        return;
    case 5:
        e->addr = c->r.a[reg] + sext(fetch16(c), SZ_W);
        return;
    case 6:
        e->addr = indexed(c, c->r.a[reg]);
        return;
    default:
        break;
    }
    switch (reg) {
    case 0:     /* (xxx).W */
        e->addr = sext(fetch16(c), SZ_W);
        e->ae_pc = c->r.pc;
        return;
    case 1:     /* (xxx).L */
        e->addr = fetch32(c);
        e->ae_pc = c->r.pc;
        return;
    case 2:     /* (d16,PC): relative to the extension word */
        e->addr = pc0 + sext(fetch16(c), SZ_W);
        e->acc = ACC_PROG;
        return;
    case 3:     /* (d8,PC,Xn) */
        e->addr = indexed(c, pc0);
        e->acc = ACC_PROG;
        return;
    default:    /* #imm (the table never sends modes 7/5-7 here) */
        e->kind = EA_IMM;
        e->addr = size == SZ_L ? fetch32(c) : fetch16(c) & sz_mask(size);
        return;
    }
}

void ea_commit(relevo_cpu *c, ea_t *e)
{
    if (e->step != 0) {
        c->r.a[e->reg] += (uint32_t)(int32_t)e->step;
        e->step = 0;
    }
}

uint32_t ea_read(relevo_cpu *c, ea_t *e)
{
    switch (e->kind) {
    case EA_DREG:
        return c->r.d[e->addr] & sz_mask(e->size);
    case EA_AREG:
        return c->r.a[e->addr] & sz_mask(e->size);
    case EA_IMM:
        return e->addr;
    default:
        break;
    }
    if (e->step < 0) {
        cycles(c, 2);       /* -(An) on a read */
        ea_commit(c, e);
    } else if (e->step > 0 && e->size != SZ_L) {
        ea_commit(c, e);
    }
    c->ae_pc = e->ae_pc;
    uint32_t v = cpu_read(c, e->addr, e->size, e->acc);
    ea_commit(c, e);
    return v;
}

void ea_write(relevo_cpu *c, ea_t *e, uint32_t v)
{
    switch (e->kind) {
    case EA_DREG:
        set_dreg(c, e->addr, v, e->size);
        return;
    case EA_AREG:
        c->r.a[e->addr] = sext(v, e->size);
        return;
    case EA_IMM:
        return;
    default:
        break;
    }
    if (e->step < 0 && e->size != SZ_L)
        ea_commit(c, e);
    c->ae_pc = e->ae_pc;
    if (e->size == SZ_L && e->mode == 4) {
        /* -(An): low word first */
        cpu_write16(c, e->addr + 2, v & 0xFFFF);
        cpu_write16(c, e->addr, v >> 16);
    } else {
        cpu_write(c, e->addr, v, e->size);
    }
    ea_commit(c, e);
}
