/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT's 68k decoder (see decode.h). The encodings are those of
 * M68000PRM chapter 4 and, for the 68020's extension words, the 32-bit
 * branch displacement and its new instructions, MC68020UM. The effective
 * address classes are the interpreter's, and an opcode the interpreter's
 * table rejects on this model is never translated.
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

static void ea_none(jit_ea *e)
{
    memset(e, 0, sizeof *e);
    e->base = e->index = -1;
}

/* The 68020's full extension word (MC68020UM 2.2), as the interpreter
 * reads it (interp/ea.c): the reserved encodings are its business. */
static bool full_format(reader *r, jit_ea *e, int base, uint32_t pc_ext,
                        uint32_t ext)
{
    unsigned iis = ext & 7;
    bool is = (ext & 0x0040) != 0;
    if ((ext & 0x0030) == 0 || iis == 4 || (is && iis > 4))
        return false;
    uint32_t bd = 0, od = 0;
    if (((ext >> 4) & 3) == 2)
        bd = sx16(rd16(r));
    else if (((ext >> 4) & 3) == 3)
        bd = rd32(r);
    if ((iis & 3) == 2)
        od = sx16(rd16(r));
    else if ((iis & 3) == 3)
        od = rd32(r);
    e->kind = JEA_MEM;
    e->base = -1;
    e->disp = bd;
    if (!(ext & 0x0080)) {
        if (base >= 0)
            e->base = (int8_t)base;
        else
            e->disp = pc_ext + bd;
    }
    if (!is) {
        e->index = (int8_t)(((ext >> 12) & 7) + ((ext & 0x8000) ? 8 : 0));
        e->index_long = (ext & 0x0800) != 0;
        e->scale = (uint8_t)((ext >> 9) & 3);
    }
    e->ind = (uint8_t)(iis == 0 ? 0 : (iis & 4) ? 2 : 1);
    e->od = od;
    return true;
}

/* (d8,An,Xn) and (d8,PC,Xn): the brief extension word or, from the 68020
 * on, the full one. `base` is An as 8-15, or -1 for the PC (whose value,
 * that of the extension word, is `pc_ext`). */
static bool indexed(reader *r, jit_ea *e, int base, uint32_t pc_ext)
{
    relevo_cpu *c = r->c;
    uint32_t ext = rd16(r);
    bool m020 = c->model >= RELEVO_68020;
    if (m020 && (ext & 0x0100))
        return full_format(r, e, base, pc_ext, ext);
    e->kind = JEA_MEM;
    e->base = (int8_t)base;
    e->index = (int8_t)(((ext >> 12) & 7) + ((ext & 0x8000) ? 8 : 0));
    e->index_long = (ext & 0x0800) != 0;
    e->scale = (uint8_t)(m020 ? (ext >> 9) & 3 : 0);
    e->disp = (base < 0 ? pc_ext : 0) + sx8(ext);
    return true;
}

static bool dec_ea(reader *r, jit_ea *e, unsigned mode, unsigned reg,
                   unsigned size)
{
    ea_none(e);
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
        return indexed(r, e, JR_A(reg), 0);
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
        return indexed(r, e, -1, pc_ext);
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
    ea_none(e);
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

/* An instruction run by its interpreter handler, called from the block:
 * only register operands, no exception possible. `wregs`: what it may
 * change. */
static bool call_handler(jit_insn *in, uint16_t wregs)
{
    in->kind = JK_CALL;
    in->wregs = wregs;
    return true;
}

/* ---- Lines ------------------------------------------------------------ */

/* BTST, BCHG, BCLR, BSET: 0000 rrr 1tt <ea> with the bit number in Dn,
 * 0000 1000 tt <ea> with it in the next word. On Dn the operand is a long
 * (bit modulo 32), in memory a byte (modulo 8). */
static bool dec_bit(reader *r, jit_insn *in, uint16_t op)
{
    unsigned type = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    in->kind = JK_BIT;
    in->sub = (uint8_t)type;
    unsigned classes;
    if (op & 0x0100) {
        set_dreg_ea(&in->src, (op >> 9) & 7);
        classes = type == JB_TST ? EA_DATA : EA_DATA_ALT;
    } else {
        set_imm_ea(&in->src, rd16(r) & 0xFF);
        classes = type == JB_TST ? EA_DATA & ~(unsigned)EAB_IMM : EA_DATA_ALT;
    }
    in->size = (uint8_t)(mode == 0 ? 4 : 1);
    return dec_ea_class(r, &in->dst, mode, reg, in->size, classes);
}

/* Line 0: bit operations, and ORI, ANDI, SUBI, ADDI, EORI, CMPI to a data
 * alterable operand (CMPI also relative to the PC on the 68020). */
static bool dec_line0(reader *r, jit_insn *in, uint16_t op)
{
    static const int8_t alu[8] = { JA_OR, JA_AND, JA_SUB, JA_ADD, -1, JA_EOR,
                                   JA_CMP, -1 };
    if ((op & 0x0100) || (op & 0x0F00) == 0x0800)
        return dec_bit(r, in, op);
    unsigned size = size_of(op);
    int a = alu[(op >> 9) & 7];
    if (size == 0 || a < 0)
        return false;
    uint32_t imm = size == 4 ? rd32(r) : rd16(r) & sz_mask(size);
    in->kind = JK_ALU;
    in->alu = (uint8_t)a;
    in->size = (uint8_t)size;
    set_imm_ea(&in->src, imm);
    unsigned classes = EA_DATA_ALT;
    if (a == JA_CMP && r->c->model >= RELEVO_68020)
        classes |= EAB_PCD16 | EAB_PCD8;
    return dec_ea_class(r, &in->dst, (op >> 3) & 7, op & 7, size, classes);
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
    bool m020 = r->c->model >= RELEVO_68020;
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
    if ((op & 0xFFF8) == 0x4E50 || (op & 0xFFF8) == 0x4808) {
        in->kind = JK_LINK;         /* LINK.W, LINK.L (68020) */
        set_areg_ea(&in->dst, reg);
        set_imm_ea(&in->src, (op & 0x0040) ? sx16(rd16(r)) : rd32(r));
        return true;
    }
    if ((op & 0xFFF8) == 0x4E58) {
        in->kind = JK_UNLK;
        set_areg_ea(&in->dst, reg);
        return true;
    }
    if ((op & 0xF1C0) == 0x41C0) {
        in->kind = JK_LEA;
        in->size = 4;
        set_areg_ea(&in->dst, (op >> 9) & 7);
        return dec_ea_class(r, &in->src, mode, reg, 4, EA_CONTROL);
    }
    if ((op & 0xFFF8) == 0x4840) {
        in->kind = JK_SWAP;
        set_dreg_ea(&in->dst, reg);
        return true;
    }
    if ((op & 0xFFC0) == 0x4840) {
        in->kind = JK_PEA;
        in->size = 4;
        return dec_ea_class(r, &in->src, mode, reg, 4, EA_CONTROL);
    }
    if ((op & 0xFFF8) == 0x4880 || (op & 0xFFF8) == 0x48C0
        || (op & 0xFFF8) == 0x49C0) {
        in->kind = JK_EXT;
        in->sub = (uint8_t)((op & 0x0100) ? JE_BL
                            : (op & 0x0040) ? JE_L : JE_W);
        set_dreg_ea(&in->dst, reg);
        return true;
    }
    if ((op & 0xFB80) == 0x4880) {      /* MOVEM */
        bool to_regs = (op & 0x0400) != 0;
        in->kind = JK_MOVEM;
        in->sub = to_regs;
        in->size = (op & 0x0040) ? 4 : 2;
        in->mask = (uint16_t)rd16(r);
        if (in->mask == 0)
            return false;
        jit_ea *e = to_regs ? &in->src : &in->dst;
        unsigned classes = to_regs ? EA_CONTROL | EAB_POSTINC
                                   : EA_CONTROL_ALT | EAB_PREDEC;
        if (!dec_ea_class(r, e, mode, reg, in->size, classes))
            return false;
        e->step = 0;    /* MOVEM moves An itself */
        return true;
    }
    if ((op & 0xFF80) == 0x4C00) {      /* MULx.L, DIVx.L */
        in->kind = (op & 0x0040) ? JK_DIVL : JK_MULL;
        in->ext = (uint16_t)rd16(r);
        in->size = 4;
        return dec_ea_class(r, &in->src, mode, reg, 4, EA_DATA);
    }
    if ((op & 0xFFF8) == 0x4800)        /* NBCD Dn */
        return call_handler(in, (uint16_t)(1u << reg));
    if ((op & 0xF900) == 0x4000 && size != 0) {
        /* NEGX 40, CLR 42, NEG 44, NOT 46 */
        unsigned k = (op >> 9) & 3;
        in->size = (uint8_t)size;
        if (k == 1) {
            in->kind = JK_CLR;
        } else {
            in->kind = JK_UNARY;
            in->sub = (uint8_t)(k == 0 ? JU_NEGX : k == 2 ? JU_NEG : JU_NOT);
        }
        return dec_ea_class(r, &in->dst, mode, reg, size, EA_DATA_ALT);
    }
    if ((op & 0xFF00) == 0x4A00 && size != 0) {
        in->kind = JK_TST;
        in->size = (uint8_t)size;
        unsigned classes = EA_DATA_ALT;
        if (m020)
            classes |= EAB_PCD16 | EAB_PCD8 | EAB_IMM
                     | (size != 1 ? EAB_AREG : 0);
        return dec_ea_class(r, &in->src, mode, reg, size, classes);
    }
    return false;
}

/* ADDQ, SUBQ, DBcc and Scc. */
static bool dec_line5(reader *r, jit_insn *in, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7, size = size_of(op);
    if (size == 0) {
        in->cc = (uint8_t)((op >> 8) & 15);
        if (mode != 1) {
            in->kind = JK_SCC;
            in->size = 1;
            return dec_ea_class(r, &in->dst, mode, reg, 1, EA_DATA_ALT);
        }
        in->kind = JK_DBCC;
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

/* ADDX/SUBX (lines D/9) and CMPM (line B): 1101 xxx 1 ss 00 r yyy. */
static bool dec_xy(jit_insn *in, uint16_t op, unsigned line)
{
    unsigned size = size_of(op), rx = (op >> 9) & 7, ry = op & 7;
    bool mem = (op & 8) != 0;
    in->size = (uint8_t)size;
    if (line == 0xB) {
        if (!mem)
            return false;           /* EOR Dn,An does not exist */
        in->kind = JK_CMPM;
        int step = size == 1 ? 1 : (int)size;
        ea_none(&in->src);
        in->src.kind = JEA_MEM;
        in->src.base = (int8_t)JR_A(ry);
        in->src.step = (int8_t)(size == 1 && ry == 7 ? 2 : step);
        ea_none(&in->dst);
        in->dst.kind = JEA_MEM;
        in->dst.base = (int8_t)JR_A(rx);
        in->dst.step = (int8_t)(size == 1 && rx == 7 ? 2 : step);
        return true;
    }
    in->kind = JK_ADDX;
    in->sub = line == 0x9;
    if (!mem) {
        set_dreg_ea(&in->src, ry);
        set_dreg_ea(&in->dst, rx);
        return true;
    }
    int sy = size == 1 && ry == 7 ? 2 : (int)size;
    int sx = size == 1 && rx == 7 ? 2 : (int)size;
    ea_none(&in->src);
    in->src.kind = JEA_MEM;
    in->src.base = (int8_t)JR_A(ry);
    in->src.step = (int8_t)-sy;
    ea_none(&in->dst);
    in->dst.kind = JEA_MEM;
    in->dst.base = (int8_t)JR_A(rx);
    in->dst.step = (int8_t)-sx;
    return true;
}

/* Lines 8 (OR, DIVx, SBCD, PACK, UNPK), 9 (SUB, SUBX), B (CMP, CMPA, EOR,
 * CMPM), C (AND, MULx, ABCD, EXG), D (ADD, ADDX). */
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
        if (line == 0x8 || line == 0xC) {
            /* DIVU/DIVS, MULU/MULS .W */
            in->kind = line == 0x8 ? JK_DIV : JK_MUL;
            in->sub = opmode == 7;
            in->size = 2;
            set_dreg_ea(&in->dst, dn);
            if (!dec_ea_class(r, &in->src, mode, reg, 2, EA_DATA))
                return false;
            /* a constant 0 divisor always traps */
            return !(in->kind == JK_DIV && in->src.kind == JEA_IMM
                     && in->src.disp == 0);
        }
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
    /* Dn,<ea>, or the register-to-register forms in modes 0 and 1 */
    if (mode <= 1) {
        if (line == 0x9 || line == 0xD || (line == 0xB && mode == 1))
            return dec_xy(in, op, line);
        if ((line == 0x8 || line == 0xC) && opmode == 4 && mode == 0)
            return call_handler(in, (uint16_t)(1u << dn));  /* SBCD ABCD */
        if (line == 0x8 && (opmode == 5 || opmode == 6) && mode == 0) {
            rd16(r);                                         /* PACK UNPK */
            return call_handler(in, (uint16_t)(1u << dn));
        }
        if (line == 0xC && (opmode == 5 || opmode == 6)) {
            /* EXG Dx,Dy (01000), Ax,Ay (01001), Dx,Ay (10001) */
            unsigned k = (op >> 3) & 0x1F;
            in->kind = JK_EXG;
            if (k == 0x08) {
                set_dreg_ea(&in->src, dn);
                set_dreg_ea(&in->dst, reg);
            } else if (k == 0x09) {
                set_areg_ea(&in->src, dn);
                set_areg_ea(&in->dst, reg);
            } else if (k == 0x11) {
                set_dreg_ea(&in->src, dn);
                set_areg_ea(&in->dst, reg);
            } else {
                return false;
            }
            return true;
        }
    }
    set_dreg_ea(&in->src, dn);
    if (line == 0xB) {
        in->alu = JA_EOR;
        return dec_ea_class(r, &in->dst, mode, reg, size, EA_DATA_ALT);
    }
    return dec_ea_class(r, &in->dst, mode, reg, size, EA_MEM_ALT);
}

/* Line E: shifts and rotates; bit fields (68020) on a data register. */
static bool dec_lineE(reader *r, jit_insn *in, uint16_t op)
{
    unsigned size = size_of(op);
    if (size == 0) {
        if (op & 0x0800) {
            /* BFxxx, through the handler */
            uint32_t ext = rd16(r);
            unsigned mode = (op >> 3) & 7, type = (op >> 8) & 7;
            if (mode == 0)
                return call_handler(in, (uint16_t)(1u << (op & 7)
                                                   | 1u << ((ext >> 12) & 7)));
            /* In memory: the 5 bytes a field may cover are checked first,
             * from the address and the offset (in Dn, or 0-31). */
            bool reads_only = type == 0 || type == 1 || type == 3 || type == 5;
            if (!dec_ea_class(r, &in->dst, mode, op & 7, 1,
                              reads_only ? EA_CONTROL : EA_CONTROL_ALT))
                return false;
            call_handler(in, (uint16_t)(1u << ((ext >> 12) & 7)));
            in->sub = 1;
            in->ext = (uint16_t)ext;
            in->write = !reads_only;
            in->size = 5;
            return true;
        }
        /* <op> <ea>: a word in memory, by one */
        in->kind = JK_SHIFT;
        in->sub = (uint8_t)((op >> 9) & 3);
        in->left = (op & 0x0100) != 0;
        in->count = 1;
        in->size = 2;
        return dec_ea_class(r, &in->dst, (op >> 3) & 7, op & 7, 2,
                            EA_MEM_ALT);
    }
    if (op & 0x0020)        /* count in a register */
        return call_handler(in, (uint16_t)(1u << (op & 7)));
    unsigned ccc = (op >> 9) & 7;
    in->kind = JK_SHIFT;
    in->sub = (uint8_t)((op >> 3) & 3);
    in->left = (op & 0x0100) != 0;
    in->count = (uint8_t)(ccc ? ccc : 8);
    in->size = (uint8_t)size;
    set_dreg_ea(&in->dst, op & 7);
    return true;
}

/* ---- Checks ----------------------------------------------------------- */

/* A memory access at a constant address, checked now: it must be a plain
 * access, as the translated code makes no check for it (other than for
 * writes into translated code). The page map only changes through
 * relevo_map(), which drops every translation. */
static bool const_access_ok(relevo_cpu *c, uint32_t addr, unsigned size,
                            bool write)
{
    if (size > 1 && (addr & 1) && c->align_check)
        return false;
    uint32_t a = addr & c->addr_mask;
    uint8_t kind = c->pages[a >> RELEVO_PAGE_SHIFT];
    if (write ? kind != RELEVO_PAGE_RAM && kind != PAGE_RAM_CODE
              : !page_direct_read(kind))
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

static uint16_t ea_reg_of(const jit_ea *e)
{
    if (e->kind == JEA_DREG || e->kind == JEA_AREG)
        return (uint16_t)(1u << e->reg);
    return 0;
}

static uint16_t ea_steps(const jit_ea *e)
{
    if (e->kind == JEA_MEM && e->step != 0)
        return (uint16_t)(1u << e->base);
    return 0;
}

/* An operand in memory that is read and/or written: a constant address is
 * checked now, any other is checked by the code (and may bail). */
static bool mem_operand(relevo_cpu *c, jit_insn *in, const jit_ea *e,
                        unsigned size, bool write)
{
    if (e->kind != JEA_MEM)
        return true;
    in->cycles = (uint16_t)(in->cycles + (size == 4 ? 8 : 4));
    if (jea_mem_const(e)) {
        /* A write may still hit translated code. */
        if (write)
            in->may_bail = true;
        return const_access_ok(c, e->disp, size, write);
    }
    in->may_bail = true;
    return true;
}

/* Address-only operands (LEA, PEA, JMP, JSR): a memory indirect one reads
 * memory, which may bail. */
static void addr_operand(jit_insn *in, const jit_ea *e)
{
    if (e->ind)
        in->may_bail = true;
}

static unsigned popcount16(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

bool jit_decode(relevo_cpu *c, uint32_t pc, jit_insn *in, bool *rom)
{
    memset(in, 0, sizeof *in);
    ea_none(&in->src);
    ea_none(&in->dst);
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
        ok = dec_line0(&r, in, op);
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
    case 0xE:
        ok = dec_lineE(&r, in, op);
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
    uint16_t w = (uint16_t)(ea_steps(&in->src) | ea_steps(&in->dst));
    uint16_t a7 = 1u << JR_A(7);

    unsigned size = in->size;
    switch (in->kind) {
    case JK_MOVE:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_MOVEA:
    case JK_ADDA:
        if (in->kind == JK_ADDA && in->alu == JA_CMP)
            in->flags_def = JF_NZVC;
        else
            w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->src, size, false);
        break;
    case JK_MOVEQ:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        break;
    case JK_LEA:
        w |= ea_reg_of(&in->dst);
        addr_operand(in, &in->src);
        break;
    case JK_PEA:
        in->regs |= a7;
        w |= a7;
        in->may_bail = true;
        in->cycles = (uint16_t)(in->cycles + 8);
        break;
    case JK_ALU:
        in->flags_def = (in->alu == JA_ADD || in->alu == JA_SUB) ? JF_ALL
                                                                 : JF_NZVC;
        if (in->alu != JA_CMP)
            w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, in->alu != JA_CMP);
        if (in->dst.kind == JEA_MEM && in->alu != JA_CMP)
            in->cycles = (uint16_t)(in->cycles + (size == 4 ? 8 : 4));
        break;
    case JK_CMPM:
        in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, false);
        break;
    case JK_TST:
        in->flags_def = JF_NZVC;
        ok = mem_operand(c, in, &in->src, size, false);
        break;
    case JK_CLR:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_UNARY:
        if (in->sub == JU_NOT) {
            in->flags_def = JF_NZVC;
        } else {
            in->flags_def = JF_ALL;
            if (in->sub == JU_NEGX)
                in->flags_use = JF_X | JF_Z;
        }
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_ADDX:
        in->flags_def = JF_ALL;
        in->flags_use = JF_X | JF_Z;
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->src, size, false)
          && mem_operand(c, in, &in->dst, size, true);
        break;
    case JK_SHIFT:
        in->flags_def = in->sub == JS_RO ? JF_NZVC : JF_ALL;
        if (in->sub == JS_ROX)
            in->flags_use = JF_X;
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->dst, size, true);
        in->cycles = (uint16_t)(in->cycles + 2 + 2 * in->count);
        break;
    case JK_BIT:
        in->flags_def = JF_Z;
        if (in->sub != JB_TST)
            w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->dst, size, in->sub != JB_TST);
        if (in->dst.kind == JEA_IMM && in->src.kind != JEA_DREG)
            ok = false;
        break;
    case JK_MUL:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->src, size, false);
        in->cycles = (uint16_t)(in->cycles + 38);
        break;
    case JK_DIV:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        in->may_bail = true;        /* division by zero */
        ok = mem_operand(c, in, &in->src, size, false);
        in->cycles = (uint16_t)(in->cycles + 120);
        break;
    case JK_MULL:
    case JK_DIVL: {
        unsigned rl = (in->ext >> 12) & 7, rh = in->ext & 7;
        uint16_t m = (uint16_t)(1u << rl);
        if ((in->ext & 0x0400) || in->kind == JK_DIVL)
            m |= (uint16_t)(1u << rh);
        if (in->ext & 0x83F8)       /* reserved bits */
            return false;
        in->regs |= m;
        w |= m;
        in->flags_def = JF_NZVC;
        if (in->kind == JK_DIVL) {
            in->may_bail = true;
            if (in->src.kind == JEA_IMM && in->src.disp == 0)
                return false;
        }
        ok = mem_operand(c, in, &in->src, size, false);
        in->cycles = (uint16_t)(in->cycles + (in->kind == JK_DIVL ? 70 : 40));
        break;
    }
    case JK_EXT:
    case JK_SWAP:
        in->flags_def = JF_NZVC;
        w |= ea_reg_of(&in->dst);
        break;
    case JK_EXG:
        w |= ea_reg_of(&in->src) | ea_reg_of(&in->dst);
        in->cycles = (uint16_t)(in->cycles + 2);
        break;
    case JK_SCC:
        in->flags_use = cc_flags(in->cc);
        w |= ea_reg_of(&in->dst);
        ok = mem_operand(c, in, &in->dst, 1, true);
        break;
    case JK_MOVEM: {
        unsigned n = popcount16(in->mask);
        const jit_ea *e = in->sub ? &in->src : &in->dst;
        if (in->sub)
            w |= in->mask;
        if ((e->kind == JEA_MEM && e->step == 0)
            && (in->op & 0x38) == (in->sub ? 0x18 : 0x20))
            w |= (uint16_t)(1u << e->base);     /* (An)+ or -(An) */
        ok = mem_operand(c, in, e, n * size, !in->sub);
        in->cycles = (uint16_t)(in->cycles + n * (size == 4 ? 8 : 4));
        break;
    }
    case JK_LINK:
    case JK_UNLK:
        in->regs |= a7;
        w |= a7 | ea_reg_of(&in->dst);
        in->may_bail = true;
        in->cycles = (uint16_t)(in->cycles + 12);
        break;
    case JK_CALL:
        in->flags_use = JF_ALL;
        in->regs = 0;
        w = in->wregs;
        in->cycles = 0;     /* the handler counts them */
        if (in->sub == 1) {
            /* the address and the offset register, for the check */
            in->regs = ea_regs(&in->dst);
            if (in->ext & 0x0800)
                in->regs |= (uint16_t)(1u << ((in->ext >> 6) & 7));
            in->may_bail = true;
            if (jea_mem_const(&in->dst) && !(in->ext & 0x0800))
                ok = const_access_ok(c, in->dst.disp
                                        + (((unsigned)in->ext >> 6 & 31) >> 3),
                                     5, in->write);
        }
        break;
    case JK_BCC:
        in->flags_use = cc_flags(in->cc);
        in->ends_block = true;
        in->cycles = (uint16_t)(in->cycles + 6);
        break;
    case JK_DBCC:
        in->flags_use = cc_flags(in->cc);
        in->ends_block = true;
        w |= ea_reg_of(&in->dst);
        in->cycles = (uint16_t)(in->cycles + 6);
        break;
    case JK_BSR:
    case JK_JSR:
    case JK_RTS:
        in->regs |= a7;
        w |= a7;
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
    in->wregs = w;
    return ok;
}
