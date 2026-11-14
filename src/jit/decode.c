/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT's 68k decoder (see decode.h). The encodings are those of
 * M68000PRM chapter 4 and, for the brief extension word's scale and the
 * 32-bit branch displacement, MC68020UM. The effective address classes are
 * the interpreter's, and an opcode the interpreter's table rejects on this
 * model is never translated.
 */
#include <string.h>

#include "decode.h"
#include "../interp/interp.h"

typedef struct reader {
    relevo_cpu *c;
    uint32_t pc;
    bool ok;
    bool *rom;
} reader;

static uint32_t rd16(reader *r)
{
    if (!r->ok)
        return 0;
    relevo_cpu *c = r->c;
    uint32_t a = r->pc & c->addr_mask;
    uint8_t kind = c->pages[a >> RELEVO_PAGE_SHIFT];
    if (!page_direct_read(kind)
        || (uint64_t)a + 2 > (uint64_t)c->addr_mask + 1) {
        r->ok = false;
        return 0;
    }
    if (kind != RELEVO_PAGE_ROM)
        *r->rom = false;
    r->pc += 2;
    return (uint32_t)c->mem[a] << 8 | c->mem[a + 1];
}

static uint32_t rd32(reader *r)
{
    uint32_t hi = rd16(r);
    return hi << 16 | rd16(r);
}

static uint32_t sx8(uint32_t v)  { return (uint32_t)(int32_t)(int8_t)(uint8_t)v; }
static uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(uint16_t)v; }

/* (d8,An,Xn) and (d8,PC,Xn): the brief extension word. The full format
 * (68020 on, bit 8) is left to the interpreter. */
static bool brief(reader *r, jit_ea *e, int base, uint32_t extra)
{
    relevo_cpu *c = r->c;
    uint32_t ext = rd16(r);
    bool m020 = c->model >= RELEVO_68020;
    if (m020 && (ext & 0x0100))
        return false;
    e->kind = JEA_MEM;
    e->base = (int8_t)base;
    e->index = (int8_t)(((ext >> 12) & 7) + ((ext & 0x8000) ? 8 : 0));
    e->index_long = (ext & 0x0800) != 0;
    e->scale = (uint8_t)(m020 ? (ext >> 9) & 3 : 0);
    e->disp = extra + sx8(ext);
    return true;
}

static bool dec_ea(reader *r, jit_ea *e, unsigned mode, unsigned reg,
                   unsigned size)
{
    memset(e, 0, sizeof *e);
    e->base = e->index = -1;
    int step = (size == 1 && reg == 7) ? 2 : (int)size;
    switch (mode) {
    case 0:
        e->kind = JEA_DREG;
        e->reg = (uint8_t)reg;
        return true;
    case 1:
        e->kind = JEA_AREG;
        e->reg = (uint8_t)JR_A(reg);
        return true;
    case 2:
    case 3:
    case 4:
        e->kind = JEA_MEM;
        e->base = (int8_t)JR_A(reg);
        e->step = (int8_t)(mode == 3 ? step : mode == 4 ? -step : 0);
        return true;
    case 5:
        e->kind = JEA_MEM;
        e->base = (int8_t)JR_A(reg);
        e->disp = sx16(rd16(r));
        return true;
    case 6:
        return brief(r, e, JR_A(reg), 0);
    default:
        break;
    }
    uint32_t pc_ext = r->pc;
    switch (reg) {
    case 0:
        e->kind = JEA_MEM;
        e->disp = sx16(rd16(r));
        return true;
    case 1:
        e->kind = JEA_MEM;
        e->disp = rd32(r);
        return true;
    case 2:
        e->kind = JEA_MEM;
        e->disp = pc_ext + sx16(rd16(r));
        return true;
    case 3:
        return brief(r, e, -1, pc_ext);
    case 4:
        e->kind = JEA_IMM;
        e->disp = size == 4 ? rd32(r) : rd16(r) & sz_mask(size);
        return true;
    default:
        return false;
    }
}

static bool dec_ea_class(reader *r, jit_ea *e, unsigned mode, unsigned reg,
                         unsigned size, unsigned classes)
{
    if (!ea_valid(mode, reg, classes))
        return false;
    return dec_ea(r, e, mode, reg, size);
}

static void set_dreg_ea(jit_ea *e, unsigned reg)
{
    memset(e, 0, sizeof *e);
    e->base = e->index = -1;
    e->kind = JEA_DREG;
    e->reg = (uint8_t)reg;
}

static void set_areg_ea(jit_ea *e, unsigned reg)
{
    set_dreg_ea(e, reg);
    e->kind = JEA_AREG;
    e->reg = (uint8_t)JR_A(reg);
}

static void set_imm_ea(jit_ea *e, uint32_t v)
{
    set_dreg_ea(e, 0);
    e->kind = JEA_IMM;
    e->disp = v;
}

/* CCR bits a condition reads. */
static uint8_t cc_flags(unsigned cc)
{
    switch (cc & 15) {
    case 0: case 1:   return 0;
    case 2: case 3:   return JF_C | JF_Z;
    case 4: case 5:   return JF_C;
    case 6: case 7:   return JF_Z;
    case 8: case 9:   return JF_V;
    case 10: case 11: return JF_N;
    case 12: case 13: return JF_N | JF_V;
    default:          return JF_N | JF_V | JF_Z;
    }
}

static unsigned size_of(uint16_t op)
{
    return size_field(op);  /* 0 for the 11 encoding */
}

/* ---- Lines ------------------------------------------------------------ */

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI to a data alterable operand. */
static bool dec_immediate(reader *r, jit_insn *in, uint16_t op)
{
    static const int8_t alu[8] = { JA_OR, JA_AND, JA_SUB, JA_ADD, -1, JA_EOR,
                                   JA_CMP, -1 };
    unsigned size = size_of(op);
    int a = alu[(op >> 9) & 7];
    if ((op & 0x0100) || size == 0 || a < 0)
        return false;
    uint32_t imm = size == 4 ? rd32(r) : rd16(r) & sz_mask(size);
    in->kind = JK_ALU;
    in->alu = (uint8_t)a;
    in->size = (uint8_t)size;
    set_imm_ea(&in->src, imm);
    return dec_ea_class(r, &in->dst, (op >> 3) & 7, op & 7, size,
                        EA_DATA_ALT);
}

static bool dec_move(reader *r, jit_insn *in, uint16_t op)
{
    static const uint8_t sizes[4] = { 0, 1, 4, 2 };
    unsigned size = sizes[(op >> 12) & 3];
    unsigned dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    in->size = (uint8_t)size;
    if (!dec_ea_class(r, &in->src, (op >> 3) & 7, op & 7, size,
                      size == 1 ? EA_DATA : EA_ALL))
        return false;
    if (dmode == 1) {
        if (size == 1)
            return false;
        in->kind = JK_MOVEA;
        set_areg_ea(&in->dst, dreg);
        return true;
    }
    in->kind = JK_MOVE;
    return dec_ea_class(r, &in->dst, dmode, dreg, size, EA_DATA_ALT);
}

static bool dec_line4(reader *r, jit_insn *in, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7, size = size_of(op);
    if (op == 0x4E71) {
        in->kind = JK_NOP;
        return true;
    }
    if (op == 0x4E75) {
        in->kind = JK_RTS;
        return true;
    }
    if ((op & 0xFF80) == 0x4E80) {      /* JSR 4E80, JMP 4EC0 */
        in->kind = (op & 0x0040) ? JK_JMP : JK_JSR;
        in->size = 4;
        return dec_ea_class(r, &in->src, mode, reg, 4, EA_CONTROL);
    }
    if ((op & 0xF1C0) == 0x41C0) {
        in->kind = JK_LEA;
        in->size = 4;
        set_areg_ea(&in->dst, (op >> 9) & 7);
        return dec_ea_class(r, &in->src, mode, reg, 4, EA_CONTROL);
    }
    if ((op & 0xFF00) == 0x4200 && size != 0) {
        in->kind = JK_CLR;
        in->size = (uint8_t)size;
        return dec_ea_class(r, &in->dst, mode, reg, size, EA_DATA_ALT);
    }
    if ((op & 0xFF00) == 0x4A00 && size != 0) {
        in->kind = JK_TST;
        in->size = (uint8_t)size;
        return dec_ea_class(r, &in->src, mode, reg, size, EA_DATA_ALT);
    }
    return false;
}

/* ADDQ, SUBQ and DBcc (Scc is left to the interpreter). */
static bool dec_line5(reader *r, jit_insn *in, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7, size = size_of(op);
    if (size == 0) {
        if (mode != 1)
            return false;
        in->kind = JK_DBCC;
        in->cc = (uint8_t)((op >> 8) & 15);
        in->target = in->pc + 2 + sx16(rd16(r));
        set_dreg_ea(&in->dst, reg);
        return (in->target & 1) == 0;
    }
    uint32_t q = (op >> 9) & 7;
    if (q == 0)
        q = 8;
    in->alu = (op & 0x0100) ? JA_SUB : JA_ADD;
    if (mode == 1) {
        if (size == 1)
            return false;
        in->kind = JK_ADDA;     /* the whole An, no flags */
        in->size = 4;
        set_imm_ea(&in->src, q);
        set_areg_ea(&in->dst, reg);
        return true;
    }
    in->kind = JK_ALU;
    in->size = (uint8_t)size;
    set_imm_ea(&in->src, q);
    return dec_ea_class(r, &in->dst, mode, reg, size, EA_DATA_ALT);
}

static bool dec_branch(reader *r, jit_insn *in, uint16_t op)
{
    uint32_t base = in->pc + 2, disp = op & 0xFF;
    if (disp == 0)
        in->target = base + sx16(rd16(r));
    else if (disp == 0xFF && r->c->model >= RELEVO_68020)
        in->target = base + rd32(r);
    else
        in->target = base + sx8(disp);
    if (in->target & 1)
        return false;   /* always an address error: the interpreter's */
    in->cc = (uint8_t)((op >> 8) & 15);
    in->kind = in->cc == 1 ? JK_BSR : JK_BCC;
    return true;
}

/* Lines 8 (OR), 9 (SUB), B (CMP, CMPA, EOR), C (AND), D (ADD). */
static bool dec_alu_line(reader *r, jit_insn *in, uint16_t op)
{
    unsigned line = op >> 12, opmode = (op >> 6) & 7;
    unsigned mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
    bool arith = line == 0x9 || line == 0xD || line == 0xB;
    uint8_t alu;
    switch (line) {
    case 0x8: alu = JA_OR; break;
    case 0x9: alu = JA_SUB; break;
    case 0xB: alu = JA_CMP; break;
    case 0xC: alu = JA_AND; break;
    default:  alu = JA_ADD; break;
    }
    in->alu = alu;
    if (opmode == 3 || opmode == 7) {
        if (!arith)
            return false;           /* MULx, DIVx */
        unsigned size = opmode == 7 ? 4 : 2;
        in->kind = JK_ADDA;
        in->size = (uint8_t)size;
        set_areg_ea(&in->dst, dn);
        return dec_ea_class(r, &in->src, mode, reg, size, EA_ALL);
    }
    unsigned size = (unsigned[]){ 1, 2, 4 }[opmode & 3];
    in->kind = JK_ALU;
    in->size = (uint8_t)size;
    if (opmode < 3) {
        /* <ea>,Dn */
        set_dreg_ea(&in->dst, dn);
        unsigned classes = (arith && size != 1) ? EA_ALL : EA_DATA;
        return dec_ea_class(r, &in->src, mode, reg, size, classes);
    }
    /* Dn,<ea> */
    set_dreg_ea(&in->src, dn);
    if (line == 0xB) {
        in->alu = JA_EOR;
        return dec_ea_class(r, &in->dst, mode, reg, size, EA_DATA_ALT);
    }
    return dec_ea_class(r, &in->dst, mode, reg, size, EA_MEM_ALT);
}

/* ---- Checks ----------------------------------------------------------- */

/* A memory access at a constant address, checked now: it must be a plain
 * access, as the translated code makes no check for it. The page map only
 * changes through relevo_map(), which drops every translation. */
static bool const_access_ok(relevo_cpu *c, uint32_t addr, unsigned size,
                            bool write)
{
    if (size > 1 && (addr & 1) && c->align_check)
        return false;
    uint32_t a = addr & c->addr_mask;
    uint8_t kind = c->pages[a >> RELEVO_PAGE_SHIFT];
    if (write ? kind != RELEVO_PAGE_RAM : !page_direct_read(kind))
        return false;
    return in_page(a, size);
}

static uint16_t ea_regs(const jit_ea *e)
{
    uint16_t m = 0;
    if (e->kind == JEA_DREG || e->kind == JEA_AREG)
        m |= (uint16_t)(1u << e->reg);
    if (e->kind == JEA_MEM) {
        if (e->base >= 0)
            m |= (uint16_t)(1u << e->base);
        if (e->index >= 0)
            m |= (uint16_t)(1u << e->index);
    }
    return m;
}

/* An operand in memory that is read and/or written: a constant address is
 * checked now, any other is checked by the code (and may bail). */
static bool mem_operand(relevo_cpu *c, jit_insn *in, const jit_ea *e,
                        unsigned size, bool write)
{
    if (e->kind != JEA_MEM)
        return true;
    if (jea_mem_const(e)) {
        /* A write may still hit the block's own code (translate.c). */
        if (write)
            in->may_bail = true;
        return const_access_ok(c, e->disp, size, write);
    }
    in->may_bail = true;
    in->cycles = (uint16_t)(in->cycles + (size == 4 ? 8 : 4));
    return true;
}

bool jit_decode(relevo_cpu *c, uint32_t pc, jit_insn *in, bool *rom)
{
    memset(in, 0, sizeof *in);
    in->src.base = in->src.index = in->dst.base = in->dst.index = -1;
    in->pc = pc;
    if (pc & 1)
        return false;
    reader r = { c, pc, true, rom };
    uint16_t op = (uint16_t)rd16(&r);
    if (!r.ok)
        return false;
    relevo_handler h = c->table[op];
    if (h == op_illegal || h == op_line_a || h == op_line_f)
        return false;
    in->op = op;

    bool ok;
    switch (op >> 12) {
    case 0x0:
        ok = dec_immediate(&r, in, op);
        break;
    case 0x1: case 0x2: case 0x3:
        ok = dec_move(&r, in, op);
        break;
    case 0x4:
        ok = dec_line4(&r, in, op);
        break;
    case 0x5:
        ok = dec_line5(&r, in, op);
        break;
    case 0x6:
        ok = dec_branch(&r, in, op);
        break;
    case 0x7:
        ok = (op & 0x0100) == 0;
        in->kind = JK_MOVEQ;
        in->size = 4;
        set_imm_ea(&in->src, sx8(op));
        set_dreg_ea(&in->dst, (op >> 9) & 7);
        break;
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD:
        ok = dec_alu_line(&r, in, op);
        break;
    default:
        ok = false;
        break;
    }
    if (!ok || !r.ok)
        return false;
    in->len = (uint8_t)(r.pc - pc);
    if ((uint64_t)(pc & c->addr_mask) + in->len
        > (uint64_t)c->addr_mask + 1)
        return false;
    in->cycles = (uint16_t)(2 * in->len);
    in->regs = (uint16_t)(ea_regs(&in->src) | ea_regs(&in->dst));

    unsigned size = in->size;
    switch (in->kind) {
    case JK_MOVE:
        in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_MOVEA:
    case JK_ADDA:
        if (in->kind == JK_ADDA && in->alu == JA_CMP)
            in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false);
        break;
    case JK_MOVEQ:
        in->flags_def = JF_NZVC;
        break;
    case JK_ALU:
        in->flags_def = (in->alu == JA_ADD || in->alu == JA_SUB) ? JF_ALL
                                                                 : JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, in->alu != JA_CMP);
        if (in->dst.kind == JEA_MEM && in->alu != JA_CMP)
            in->cycles = (uint16_t)(in->cycles + (size == 4 ? 8 : 4));
        break;
    case JK_TST:
        in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false);
        break;
    case JK_CLR:
        in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_BCC:
        in->flags_use = cc_flags(in->cc);
        in->ends_block = true;
        in->cycles = (uint16_t)(in->cycles + 6);
        break;
    case JK_DBCC:
        in->flags_use = cc_flags(in->cc);
        in->ends_block = true;
        in->cycles = (uint16_t)(in->cycles + 6);
        break;
    case JK_BSR:
    case JK_JSR:
    case JK_RTS:
        in->regs |= 1u << JR_A(7);
        in->may_bail = true;
        in->ends_block = true;
        in->cycles = (uint16_t)(in->cycles + 16);
        if (in->kind == JK_JSR && jea_mem_const(&in->src)
            && (in->src.disp & 1))
            ok = false;
        break;
    case JK_JMP:
        in->ends_block = true;
        in->cycles = (uint16_t)(in->cycles + 6);
        if (jea_mem_const(&in->src))
            ok = (in->src.disp & 1) == 0;
        else
            in->may_bail = true;    /* odd target */
        break;
    default:
        break;
    }
    return ok;
}
