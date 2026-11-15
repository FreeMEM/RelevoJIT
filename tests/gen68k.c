/* SPDX-License-Identifier: MPL-2.0
 *
 * The random 68k code and state generator shared by the differential tests
 * (jitdiff and sysdiff, the JIT against the interpreter). See gen68k.h.
 */
#include "gen68k.h"

relevo_model model = RELEVO_68000;
relevo_fpu gen_fpu = RELEVO_FPU_NONE;

/* ---- Random numbers --------------------------------------------------- */

uint64_t rng_state = 0x123456789ABCDEFull;

uint32_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

uint32_t rn(uint32_t n)
{
    return rnd() % n;
}

bool chance(unsigned percent)
{
    return rn(100) < percent;
}

/* ---- Instructions ----------------------------------------------------- */

uint16_t words[64];
unsigned nwords;

void w16(uint32_t v)
{
    if (nwords < 64)
        words[nwords++] = (uint16_t)v;
}

void w32(uint32_t v)
{
    w16(v >> 16);
    w16(v);
}

/* An address an operand may use: mostly the data window, sometimes the
 * special pages, sometimes odd. */
uint32_t rnd_addr(void)
{
    uint32_t a;
    switch (rn(18)) {
    case 0:  a = ROM_PAGE + rn(RELEVO_PAGE_SIZE); break;
    case 1:  a = HOST_PAGE + rn(RELEVO_PAGE_SIZE); break;
    case 2:  a = NONE_PAGE + rn(RELEVO_PAGE_SIZE); break;
    case 3:  a = WIN - 4 + rn(8); break;
    case 4:  a = CODE + rn(0x900); break;           /* the code itself */
    case 5:  a = WIN + 0x1000 - 4 + rn(8); break;   /* across 4 KB */
    default: a = WIN + rn(WIN_SIZE); break;
    }
    if (!chance(15))
        a &= ~1u;
    return a;
}

uint32_t rnd_value(void)
{
    switch (rn(6)) {
    case 0:  return 0;
    case 1:  return (uint32_t)(int32_t)(int8_t)rnd();
    case 2:  return rnd() & 0xFFFF;
    case 3:  return 0x80000000u >> rn(32);
    case 4:  return (uint32_t)-(int32_t)rn(4);
    default: return rnd();
    }
}

/* An effective address of `classes` (bits as in interp.h: Dn An (An) (An)+
 * -(An) d16 d8 abs.W abs.L d16PC d8PC #imm); returns mode << 3 | reg and
 * appends its extension words. */
unsigned rnd_ea(unsigned classes, unsigned size)
{
    unsigned slot;
    do
        slot = rn(12);
    while (!((classes >> slot) & 1));
    unsigned reg = rn(8);
    unsigned mode = slot < 7 ? slot : 7;
    if (slot >= 7)
        reg = slot - 7;
    switch (slot) {
    case 5:     /* (d16,An) */
        w16(chance(80) ? (uint32_t)(int32_t)(int8_t)rnd() : rnd());
        break;
    case 6:     /* (d8,An,Xn) */
    case 10: {  /* (d8,PC,Xn) */
        uint32_t ext = rnd() & 0xFEFF;
        if (model >= RELEVO_68020 && chance(25)) {
            /* the full format: base and index suppression, base and
             * outer displacements, memory indirection (a few reserved
             * encodings too) */
            ext = (rnd() & 0xFEC7) | 0x0100;
            ext |= (chance(95) ? 1 + rn(3) : 0) << 4;
            if (chance(95) && (ext & 7) == 4)
                ext &= ~7u;
            w16(ext);
            unsigned bd = (ext >> 4) & 3, od = ext & 3;
            uint32_t base = chance(50) ? rnd_addr() : rnd_value();
            if (bd == 2)
                w16(chance(70) ? rn(0x40) * 4 : rnd());
            else if (bd == 3)
                w32((ext & 0x0080) ? base : rnd_value());
            if ((ext & 7) != 0 && od == 2)
                w16(rnd());
            else if ((ext & 7) != 0 && od == 3)
                w32(rnd_value());
            break;
        }
        w16(ext);
        break;
    }
    case 7:     /* (xxx).W */
        w16(chance(50) ? 0x1000 + rn(0x6000) : rnd());
        break;
    case 8:     /* (xxx).L */
        w32(rnd_addr());
        break;
    case 9:     /* (d16,PC) */
        w16((uint32_t)(int32_t)(int16_t)(rn(0x400) - 0x200));
        break;
    case 11:    /* #imm */
        if (size == 4)
            w32(rnd_value());
        else
            w16(rnd_value() & (size == 1 ? 0xFF : 0xFFFF));
        break;
    default:
        break;
    }
    return mode << 3 | reg;
}


unsigned rnd_size(void)
{
    return (unsigned[]){ 1, 2, 4 }[rn(3)];
}

unsigned size_bits(unsigned size)
{
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

/* A branch displacement from `pc` (of the opcode) staying in the code
 * area, sometimes odd. */
int32_t rnd_disp(uint32_t pc)
{
    uint32_t target = CODE + (rn(CODE_END - CODE) & ~1u);
    if (chance(3))
        target |= 1;
    if (chance(20))
        target = CODE;      /* back to the start: a loop */
    return (int32_t)(target - (pc + 2));
}

/* A register list for MOVEM. */
uint32_t rnd_mask(void)
{
    switch (rn(4)) {
    case 0:  return 1u << rn(16);
    case 1:  return rnd() & 0xFFFF;
    case 2:  return (rnd() & 0x7CFC) | 1;
    default: return rnd() & (0xFFFFu >> rn(16));
    }
}

/* An FPU opmode: mostly those the JIT runs, sometimes any. */
unsigned rnd_fop(void)
{
    static const uint8_t ops[] = { 0x00, 0x04, 0x18, 0x1A, 0x20, 0x22, 0x23,
                                   0x28, 0x38, 0x3A, 0x22, 0x23, 0x38 };
    static const uint8_t sd[] = { 0x40, 0x41, 0x44, 0x45, 0x58, 0x5A, 0x5C,
                                  0x5E, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67,
                                  0x68, 0x6C };
    if (chance(5))
        return rn(0x80);
    if (gen_fpu == RELEVO_FPU_INTERNAL && chance(25))
        return sd[rn(sizeof sd)];
    return ops[rn(sizeof ops)];
}

/* FPU instructions: FMOVE and the arithmetic between registers, from and
 * to memory or Dn in every format, FBcc, and some the JIT leaves to the
 * interpreter (control registers, FScc). */
void rnd_fpu(uint32_t pc, unsigned dn)
{
    static const uint8_t fmts[] = { 0, 1, 2, 4, 5, 6, 0, 1, 5, 2, 3 };
    static const unsigned fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };
    unsigned fmt = fmts[rn(sizeof fmts)];
    unsigned size = fmt_size[fmt];
    bool shortf = size <= 4 && fmt != 3;
    switch (rn(8)) {
    case 0:     /* FPm,FPn */
        w16(0xF200);
        w16(rn(8) << 10 | rn(8) << 7 | rnd_fop());
        break;
    case 1: case 2: case 3: {   /* <ea>,FPn */
        w16(0);
        w16(0x4000 | fmt << 10 | rn(8) << 7 | rnd_fop());
        unsigned ea;
        if (!shortf && chance(10)) {
            ea = 0x3C;                      /* #imm, 8 or 12 bytes */
            for (unsigned i = 0; i < size; i += 4)
                w32(rnd_value());
        } else {
            ea = rnd_ea(shortf ? C_DATA : C_DATA & ~(C_DREG | C_IMM),
                        size == 1 ? 1 : size == 2 ? 2 : 4);
        }
        words[0] = (uint16_t)(0xF200 | ea);
        break;
    }
    case 4: case 5: {   /* FPn,<ea> */
        w16(0);
        w16(0x6000 | fmt << 10 | rn(8) << 7 | (fmt == 3 ? rn(0x80) : 0));
        unsigned ea = rnd_ea(shortf ? C_DATAALT : C_MEMALT,
                             size == 1 ? 1 : size == 2 ? 2 : 4);
        words[0] = (uint16_t)(0xF200 | ea);
        break;
    }
    case 6: {   /* FBcc */
        unsigned pred = chance(95) ? rn(0x20) : rn(0x40);
        int32_t d = rnd_disp(pc);
        if (chance(30)) {
            w16(0xF2C0 | pred);
            w32((uint32_t)d);
        } else {
            w16(0xF280 | pred);
            w16((uint32_t)d);
        }
        break;
    }
    default:    /* FMOVE.L Dn,FPCR/FPSR, FScc Dn */
        if (chance(50)) {
            w16(0xF200 | dn);
            w16(chance(50) ? 0x8800 : 0x9000);
        } else {
            w16(0xF240 | dn);
            w16(rn(0x20));
        }
        break;
    }
}

/* The instructions R6b added to the JIT, and some it leaves to the
 * interpreter or to the handler calls. */
void rnd_insn_more(uint32_t pc, unsigned size, unsigned dn)
{
    unsigned dy = rn(8);
    bool m020 = model >= RELEVO_68020;
    switch (rn(42)) {
    case 36: case 37: case 38: case 39: case 40: case 41:
        if (gen_fpu != RELEVO_FPU_NONE)
            rnd_fpu(pc, dn);
        else
            w16(0x4E71);
        break;
    case 30: {  /* TAS */
        w16(0);
        words[0] = (uint16_t)(0x4AC0 | rnd_ea(C_DATAALT, 1));
        break;
    }
    case 31: {  /* CHK.W, CHK.L (68020) */
        bool l = m020 && chance(40);
        w16(0);
        unsigned ea = rnd_ea(C_DATA, l ? 4 : 2);
        words[0] = (uint16_t)((l ? 0x4100 : 0x4180) | dn << 9 | ea);
        break;
    }
    case 32: {  /* CAS (68020) */
        if (!m020) {
            w16(0x4E71);
            break;
        }
        unsigned s = 1 + rn(3);
        w16(0);
        w16(rn(8) << 6 | rn(8) | (chance(3) ? rnd() & 0xFE38 : 0));
        unsigned ea = rnd_ea(C_MEMALT, s == 1 ? 1 : s == 2 ? 2 : 4);
        words[0] = (uint16_t)(0x0800 | s << 9 | 0xC0 | ea);
        break;
    }
    case 33:    /* MOVEP */
        w16(0x0108 | dn << 9 | rn(4) << 6 | dy);
        w16(chance(70) ? (uint32_t)(int32_t)(int8_t)rnd() : rnd());
        break;
    case 34:    /* ABCD SBCD -(Ay),-(Ax) */
        w16((chance(50) ? 0xC108 : 0x8108) | dn << 9 | dy);
        break;
    case 35: {  /* NBCD <ea> */
        w16(0);
        words[0] = (uint16_t)(0x4800 | rnd_ea(C_DATAALT, 1));
        break;
    }
    case 0: case 1: case 2: {   /* shifts and rotates on Dn */
        unsigned ir = chance(25) ? 0x20 : 0;
        w16(0xE000 | rn(8) << 9 | rn(2) << 8 | size_bits(size) << 6 | ir
            | rn(4) << 3 | dy);
        break;
    }
    case 3: {   /* ... on a word in memory */
        w16(0);
        unsigned ea = rnd_ea(C_MEMALT, 2);
        words[0] = (uint16_t)(0xE0C0 | rn(4) << 9 | rn(2) << 8 | ea);
        break;
    }
    case 4: case 5: {   /* BTST BCHG BCLR BSET Dn,<ea> */
        unsigned t = rn(4);
        w16(0);
        unsigned ea = rnd_ea(t == 0 ? C_DATA : C_DATAALT, 1);
        words[0] = (uint16_t)(0x0100 | dn << 9 | t << 6 | ea);
        break;
    }
    case 6: {   /* ... #n,<ea> */
        unsigned t = rn(4);
        w16(0);
        w16(chance(70) ? rn(40) : rnd() & 0xFF);
        unsigned ea = rnd_ea(t == 0 ? C_DATA & ~C_IMM : C_DATAALT, 1);
        words[0] = (uint16_t)(0x0800 | t << 6 | ea);
        break;
    }
    case 7: case 8: {   /* MULU MULS DIVU DIVS .W */
        static const uint16_t op[] = { 0xC0C0, 0xC1C0, 0x80C0, 0x81C0 };
        w16(0);
        unsigned ea = rnd_ea(C_DATA, 2);
        words[0] = (uint16_t)(op[rn(4)] | dn << 9 | ea);
        break;
    }
    case 9: case 10: {  /* MULx.L DIVx.L */
        if (!m020) {
            w16(0x4E71);
            break;
        }
        bool div = chance(50);
        w16(0);
        uint32_t ext = rn(8) << 12 | rn(2) << 11 | rn(2) << 10 | rn(8);
        if (chance(3))
            ext |= 1u << (3 + rn(6));     /* reserved bits */
        w16(ext);
        unsigned ea = rnd_ea(C_DATA, 4);
        words[0] = (uint16_t)((div ? 0x4C40 : 0x4C00) | ea);
        break;
    }
    case 11: {  /* EXT.W EXT.L EXTB.L SWAP */
        static const uint16_t op[] = { 0x4880, 0x48C0, 0x49C0, 0x4840 };
        unsigned k = rn(m020 ? 4 : 3);
        w16(op[k == 2 ? 3 : k == 3 ? 2 : k] | dy);
        break;
    }
    case 12: {  /* EXG */
        static const uint16_t op[] = { 0xC140, 0xC148, 0xC188 };
        w16(op[rn(3)] | dn << 9 | dy);
        break;
    }
    case 13: case 14: {     /* NEGX NEG NOT */
        static const uint16_t op[] = { 0x4000, 0x4400, 0x4600 };
        w16(0);
        unsigned ea = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)(op[rn(3)] | size_bits(size) << 6 | ea);
        break;
    }
    case 15: case 16:   /* ADDX SUBX Dy,Dx / -(Ay),-(Ax) */
        w16((chance(50) ? 0xD100 : 0x9100) | dn << 9 | size_bits(size) << 6
            | (chance(40) ? 8 : 0) | dy);
        break;
    case 17:    /* CMPM */
        w16(0xB108 | dn << 9 | size_bits(size) << 6 | dy);
        break;
    case 18: {  /* Scc */
        w16(0);
        unsigned ea = rnd_ea(C_DATAALT, 1);
        words[0] = (uint16_t)(0x50C0 | rn(16) << 8 | ea);
        break;
    }
    case 19: case 20: {     /* MOVEM */
        bool to_regs = chance(50);
        unsigned l = rn(2);
        w16(0);
        w16(rnd_mask());
        unsigned ea = rnd_ea(to_regs ? (C_CONTROL | C_POST)
                                     : ((C_CONTROL & ~(C_PCD16 | C_PCD8))
                                        | C_PRE), l ? 4 : 2);
        words[0] = (uint16_t)((to_regs ? 0x4C80 : 0x4880) | l << 6 | ea);
        break;
    }
    case 21:    /* LINK UNLK */
        if (chance(50)) {
            w16(0x4E58 | dy);
        } else if (m020 && chance(30)) {
            w16(0x4808 | dy);
            w32((uint32_t)(int32_t)(int16_t)(rnd() & 0xFFF8));
        } else {
            w16(0x4E50 | dy);
            w16(chance(80) ? (uint32_t)-(int32_t)(rn(16) * 4) : rnd());
        }
        break;
    case 22: {  /* PEA */
        w16(0);
        words[0] = (uint16_t)(0x4840 | rnd_ea(C_CONTROL, 4));
        break;
    }
    case 23: {  /* ABCD SBCD Dy,Dx, NBCD Dn: handler calls */
        static const uint16_t op[] = { 0xC100, 0x8100, 0x4800 };
        unsigned k = rn(3);
        w16(op[k] | (k < 2 ? dn << 9 : 0) | dy);
        break;
    }
    case 24:    /* bit fields on Dn, PACK/UNPK Dx,Dy (68020) */
        if (!m020) {
            w16(0x4E71);
        } else if (chance(35)) {
            w16(0xE8C0 | rn(8) << 8 | dy);
            w16(rnd() & 0x7FFF);
        } else if (chance(55)) {
            /* in memory: offset 0-31 or a small one in Dn */
            unsigned t = rn(8);
            bool ro = t == 0 || t == 1 || t == 3 || t == 5;
            w16(0);
            w16(rnd() & 0x7FFF);
            unsigned ea = rnd_ea(ro ? C_CONTROL
                                    : C_CONTROL & ~(C_PCD16 | C_PCD8), 1);
            words[0] = (uint16_t)(0xE8C0 | t << 8 | ea);
        } else {
            w16((chance(50) ? 0x8140 : 0x8180) | dn << 9 | dy);
            w16(rnd());
        }
        break;
    case 25: {  /* TST and CMPI with the 68020's operands */
        w16(0);
        if (!m020) {
            words[0] = 0x4E71;
        } else if (chance(50)) {
            unsigned ea = rnd_ea(C_PCD16 | C_PCD8 | C_IMM
                                 | (size != 1 ? C_AREG : 0), size);
            words[0] = (uint16_t)(0x4A00 | size_bits(size) << 6 | ea);
        } else {
            if (size == 4)
                w32(rnd_value());
            else
                w16(rnd_value() & (size == 1 ? 0xFF : 0xFFFF));
            unsigned ea = rnd_ea(C_PCD16 | C_PCD8, size);
            words[0] = (uint16_t)(0x0C00 | size_bits(size) << 6 | ea);
        }
        break;
    }
    case 26: {  /* JMP/JSR through memory indirect, from the 68020 on */
        w16(0);
        unsigned ea = rnd_ea(C_CONTROL, 4);
        words[0] = (uint16_t)((chance(50) ? 0x4EC0 : 0x4E80) | ea);
        break;
    }
    default: {  /* MOVE with any operands, to fill longer blocks */
        static const unsigned code[] = { 0, 1, 3, 2 };
        w16(0);
        unsigned src = rnd_ea(size == 1 ? C_DATA : C_ALL, size);
        unsigned dst = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)(code[size == 4 ? 2 : size == 2 ? 3 : 1] << 12
                              | (dst & 7) << 9 | (dst >> 3) << 6 | src);
        break;
    }
    }
}

/* Writes one instruction at `pc` into words[]. */
void rnd_insn(uint32_t pc)
{
    nwords = 0;
    unsigned size = rnd_size();
    unsigned dn = rn(8);
    if (chance(55)) {
        rnd_insn_more(pc, size, dn);
        return;
    }
    switch (rn(27)) {
    case 0: case 1: case 2: case 3: {   /* MOVE */
        static const unsigned code[] = { 0, 1, 3, 2 };
        w16(0);
        unsigned src = rnd_ea(size == 1 ? C_DATA : C_ALL, size);
        unsigned dst = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)(code[size == 4 ? 2 : size == 2 ? 3 : 1] << 12
                              | (dst & 7) << 9 | (dst >> 3) << 6 | src);
        break;
    }
    case 4: {   /* MOVEA */
        size = chance(50) ? 2 : 4;
        w16(0);
        unsigned src = rnd_ea(C_ALL, size);
        words[0] = (uint16_t)((size == 4 ? 0x2040 : 0x3040) | dn << 9 | src);
        break;
    }
    case 5:     /* MOVEQ */
        w16(0x7000 | dn << 9 | (rnd() & 0xFF));
        break;
    case 6:     /* LEA */
        w16(0);
        words[0] = (uint16_t)(0x41C0 | dn << 9 | rnd_ea(C_CONTROL, 4));
        break;
    case 7: case 8: case 9: {   /* ADD SUB AND OR <ea>,Dn */
        static const uint16_t line[] = { 0xD000, 0x9000, 0xC000, 0x8000 };
        unsigned k = rn(4);
        bool arith = k < 2;
        w16(0);
        unsigned ea = rnd_ea(arith && size != 1 ? C_ALL : C_DATA, size);
        words[0] = (uint16_t)(line[k] | dn << 9 | size_bits(size) << 6 | ea);
        break;
    }
    case 10: case 11: {     /* ADD SUB AND OR Dn,<ea> */
        static const uint16_t line[] = { 0xD000, 0x9000, 0xC000, 0x8000 };
        w16(0);
        unsigned ea = rnd_ea(C_MEMALT, size);
        words[0] = (uint16_t)(line[rn(4)] | dn << 9
                              | (4 + size_bits(size)) << 6 | ea);
        break;
    }
    case 12: {  /* CMP */
        w16(0);
        unsigned ea = rnd_ea(size != 1 ? C_ALL : C_DATA, size);
        words[0] = (uint16_t)(0xB000 | dn << 9 | size_bits(size) << 6 | ea);
        break;
    }
    case 13: {  /* EOR */
        w16(0);
        unsigned ea = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)(0xB000 | dn << 9 | (4 + size_bits(size)) << 6
                              | ea);
        break;
    }
    case 14: {  /* ADDA SUBA CMPA */
        static const uint16_t line[] = { 0xD000, 0x9000, 0xB000 };
        bool l = chance(50);
        w16(0);
        unsigned ea = rnd_ea(C_ALL, l ? 4 : 2);
        words[0] = (uint16_t)(line[rn(3)] | dn << 9 | (l ? 7 : 3) << 6 | ea);
        break;
    }
    case 15: {  /* ADDQ SUBQ */
        w16(0);
        unsigned ea = rnd_ea(size == 1 ? C_DATAALT : C_ALT, size);
        words[0] = (uint16_t)(0x5000 | (rnd() & 0x0E00) | (rn(2) << 8)
                              | size_bits(size) << 6 | ea);
        break;
    }
    case 16: {  /* ORI ANDI SUBI ADDI EORI CMPI */
        static const uint16_t op[] = { 0x0000, 0x0200, 0x0400, 0x0600,
                                       0x0A00, 0x0C00 };
        w16(0);
        if (size == 4)
            w32(rnd_value());
        else
            w16(rnd_value() & (size == 1 ? 0xFF : 0xFFFF));
        unsigned ea = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)(op[rn(6)] | size_bits(size) << 6 | ea);
        break;
    }
    case 17: {  /* TST CLR */
        w16(0);
        unsigned ea = rnd_ea(C_DATAALT, size);
        words[0] = (uint16_t)((chance(50) ? 0x4A00 : 0x4200)
                              | size_bits(size) << 6 | ea);
        break;
    }
    case 18: case 19: {     /* Bcc BRA BSR */
        unsigned cc = rn(16);
        int32_t d = rnd_disp(pc);
        if (d >= -128 && d <= 127 && d != 0 && d != -1 && chance(60)) {
            w16(0x6000 | cc << 8 | (d & 0xFF));
        } else if (model >= RELEVO_68020 && chance(20)) {
            w16(0x60FF | cc << 8);
            w32((uint32_t)d);
        } else {
            w16(0x6000 | cc << 8);
            w16((uint32_t)d);
        }
        break;
    }
    case 20:    /* DBcc */
        w16(0x50C8 | rn(16) << 8 | dn);
        w16((uint32_t)rnd_disp(pc));
        break;
    case 21: {  /* JMP JSR */
        w16(0);
        unsigned ea = rnd_ea(C_CONTROL, 4);
        words[0] = (uint16_t)((chance(50) ? 0x4EC0 : 0x4E80) | ea);
        break;
    }
    case 22:
        w16(chance(50) ? 0x4E75 : 0x4E71);  /* RTS NOP */
        break;
    default: {  /* some the JIT leaves to the interpreter */
        static const uint16_t other[] = {
            0x4840,     /* SWAP D0 */
            0x4880,     /* EXT.W D0 */
            0xC0C1,     /* MULU D1,D0 */
            0x4480,     /* NEG.L D0 */
            0xD181,     /* ADDX.L D1,D0 */
            0x5BC0,     /* SMI D0 */
            0xE388,     /* LSL.L #1,D0 */
            0x4E4F,     /* TRAP #15 */
        };
        w16(other[rn(sizeof other / sizeof *other)] | (chance(50) ? 0 : dn));
        break;
    }
    }
}

/* ---- Memory and FPU values --------------------------------------------- */

uint8_t init_mem[MEM_SIZE];

void put16(uint8_t *m, uint32_t a, uint32_t v)
{
    m[a] = (uint8_t)(v >> 8);
    m[a + 1] = (uint8_t)v;
}

void put32(uint8_t *m, uint32_t a, uint32_t v)
{
    put16(m, a, v >> 16);
    put16(m, a + 2, v);
}

void setup_static(void)
{
    /* Vectors to a handler of NOPs ending in a loop. */
    for (uint32_t v = 0; v < 256; v++)
        put32(init_mem, v * 4, HANDLER);
    for (uint32_t a = HANDLER; a < HANDLER + 0x40; a += 2)
        put16(init_mem, a, 0x4E71);
    put16(init_mem, HANDLER + 0x40, 0x60BE);    /* BRA.S HANDLER */
    for (uint32_t a = ROM_PAGE; a < ROM_PAGE + RELEVO_PAGE_SIZE; a++)
        init_mem[a] = (uint8_t)(a * 7);
}

/* A random extended value: zeros, infinities, NaNs, denormals, small
 * integers and any normal number. */
relevo_fpreg rnd_fp(void)
{
    relevo_fpreg f;
    uint64_t m = (uint64_t)rnd() << 32 | rnd();
    unsigned sign = chance(50) ? 0x8000u : 0;
    switch (rn(8)) {
    case 0:
        f.sign_exponent = (uint16_t)sign;
        f.mantissa = 0;
        break;
    case 1:
        f.sign_exponent = (uint16_t)(sign | 0x7FFF);
        f.mantissa = chance(70) ? 0 : m;
        break;
    case 2:
        f.sign_exponent = (uint16_t)sign;
        f.mantissa = m >> rn(64);
        break;
    case 3: case 4:
        f.sign_exponent = (uint16_t)(sign | (0x3FFF + rn(16)));
        f.mantissa = (m | 1ull << 63) & ~((1ull << (48 + rn(15))) - 1);
        break;
    default:
        f.sign_exponent = (uint16_t)(sign | (0x3FFF + rn(0x200) - 0x100));
        f.mantissa = m | 1ull << 63;
        if (chance(3))
            f.sign_exponent = (uint16_t)(sign | rn(0x7FFF));
        break;
    }
    return f;
}
