/* SPDX-License-Identifier: MPL-2.0
 *
 * Differential test of the JIT against the interpreter.
 *
 *   jitdiff [-m 68000|68010|68020|68040] [-n CASES] [-k MAXLEN] [-s SEED]
 *           [-x]
 *
 * Each case writes 1 to MAXLEN random instructions (mostly of the kinds the
 * JIT translates, with random operands, the 68020's full extension word
 * included, and some it does not; operands also point into the code), random
 * registers, flags and data, and runs them on two CPUs, one per engine,
 * with identical memories, for as many instructions as were written (the
 * flow may go anywhere: exceptions, random jumps, data run as code). Then
 * it compares registers, SR, PC, the instruction count, the run status,
 * the accesses to a host page and memory (the windows the operands
 * reach every case, all 16 MB every 256 cases). -x makes the JIT use
 * BSWAP instead of MOVBE. -k 1 tests one instruction at a time. Exits
 * with 1 at the first difference, which it prints.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relevo.h"
#include "../src/jit/jit.h"

#define MEM_SIZE    0x1000000u
#define CODE        0x10000u        /* where the instructions go */
#define CODE_END    0x11000u
#define WIN         0x40000u        /* data the registers point into */
#define WIN_SIZE    0x2000u
#define HANDLER     0x800u          /* every exception vector */
#define ROM_PAGE    0x200000u
#define HOST_PAGE   0x210000u
#define NONE_PAGE   0x220000u

/* ---- Random numbers --------------------------------------------------- */

static uint64_t rng_state = 0x123456789ABCDEFull;

static uint32_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint32_t rn(uint32_t n)
{
    return rnd() % n;
}

static bool chance(unsigned percent)
{
    return rn(100) < percent;
}

/* ---- The two machines ------------------------------------------------- */

#define LOG_MAX 64

typedef struct access {
    uint32_t addr, value;
    unsigned size;
    bool write;
} access;

typedef struct machine {
    relevo_cpu *cpu;
    uint8_t *mem;
    access log[LOG_MAX];
    unsigned nlog;
} machine;

static uint32_t host_read(void *ctx, uint32_t addr, unsigned size)
{
    machine *m = ctx;
    uint32_t v = (addr * 0x9E3779B1u) ^ size;
    if (m->nlog < LOG_MAX)
        m->log[m->nlog++] = (access){ addr, v, size, false };
    return v;
}

static void host_write(void *ctx, uint32_t addr, uint32_t v, unsigned size)
{
    machine *m = ctx;
    if (m->nlog < LOG_MAX)
        m->log[m->nlog++] = (access){ addr, v, size, true };
}

static relevo_model model = RELEVO_68000;

static bool machine_init(machine *m, relevo_engine engine)
{
    memset(m, 0, sizeof *m);
    m->mem = calloc(1, MEM_SIZE);
    if (m->mem == NULL)
        return false;
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.engine = engine;
    cfg.memory = m->mem;
    cfg.memory_size = MEM_SIZE;
    cfg.host.ctx = m;
    cfg.host.read = host_read;
    cfg.host.write = host_write;
    m->cpu = relevo_create(&cfg);
    if (m->cpu == NULL)
        return false;
    relevo_map(m->cpu, ROM_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_ROM);
    relevo_map(m->cpu, HOST_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST);
    relevo_map(m->cpu, NONE_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_NONE);
    return true;
}

/* ---- Instructions ----------------------------------------------------- */

static uint16_t words[64];
static unsigned nwords;

static void w16(uint32_t v)
{
    if (nwords < 64)
        words[nwords++] = (uint16_t)v;
}

static void w32(uint32_t v)
{
    w16(v >> 16);
    w16(v);
}

/* An address an operand may use: mostly the data window, sometimes the
 * special pages, sometimes odd. */
static uint32_t rnd_addr(void)
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

static uint32_t rnd_value(void)
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
static unsigned rnd_ea(unsigned classes, unsigned size)
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

enum {
    C_DREG = 1 << 0, C_AREG = 1 << 1, C_IND = 1 << 2, C_POST = 1 << 3,
    C_PRE = 1 << 4, C_D16 = 1 << 5, C_D8 = 1 << 6, C_ABSW = 1 << 7,
    C_ABSL = 1 << 8, C_PCD16 = 1 << 9, C_PCD8 = 1 << 10, C_IMM = 1 << 11,
    C_ALL = 0xFFF,
    C_DATA = C_ALL & ~C_AREG,
    C_MEMALT = C_IND | C_POST | C_PRE | C_D16 | C_D8 | C_ABSW | C_ABSL,
    C_DATAALT = C_MEMALT | C_DREG,
    C_ALT = C_DATAALT | C_AREG,
    C_CONTROL = C_IND | C_D16 | C_D8 | C_ABSW | C_ABSL | C_PCD16 | C_PCD8
};

static void rnd_insn_more(uint32_t pc, unsigned size, unsigned dn);

static unsigned rnd_size(void)
{
    return (unsigned[]){ 1, 2, 4 }[rn(3)];
}

static unsigned size_bits(unsigned size)
{
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

/* A branch displacement from `pc` (of the opcode) staying in the code
 * area, sometimes odd. */
static int32_t rnd_disp(uint32_t pc)
{
    uint32_t target = CODE + (rn(CODE_END - CODE) & ~1u);
    if (chance(3))
        target |= 1;
    if (chance(20))
        target = CODE;      /* back to the start: a loop */
    return (int32_t)(target - (pc + 2));
}

/* A register list for MOVEM. */
static uint32_t rnd_mask(void)
{
    switch (rn(4)) {
    case 0:  return 1u << rn(16);
    case 1:  return rnd() & 0xFFFF;
    case 2:  return (rnd() & 0x7CFC) | 1;
    default: return rnd() & (0xFFFFu >> rn(16));
    }
}

/* The instructions R6b added to the JIT, and some it leaves to the
 * interpreter or to the handler calls. */
static void rnd_insn_more(uint32_t pc, unsigned size, unsigned dn)
{
    unsigned dy = rn(8);
    bool m020 = model >= RELEVO_68020;
    (void)pc;
    switch (rn(30)) {
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
static void rnd_insn(uint32_t pc)
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

/* ---- A case ----------------------------------------------------------- */

static uint8_t init_mem[MEM_SIZE];

static void put16(uint8_t *m, uint32_t a, uint32_t v)
{
    m[a] = (uint8_t)(v >> 8);
    m[a + 1] = (uint8_t)v;
}

static void put32(uint8_t *m, uint32_t a, uint32_t v)
{
    put16(m, a, v >> 16);
    put16(m, a + 2, v);
}

static void setup_static(void)
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

typedef struct state {
    uint32_t d[8], a[8], pc, usp, isp, msp;
    uint16_t sr;
} state;

static void load_state(machine *m, const state *s)
{
    relevo_reset(m->cpu);   /* out of a double bus fault */
    relevo_regs *r = relevo_get_regs(m->cpu);
    memcpy(r->d, s->d, sizeof r->d);
    memcpy(r->a, s->a, sizeof r->a);
    r->usp = s->usp;
    r->isp = s->isp;
    r->msp = s->msp;
    r->pc = s->pc;
    r->sr = s->sr;
    r->vbr = 0;
    m->nlog = 0;
}

static unsigned long cases;

static void dump_code(uint32_t pc, uint32_t end)
{
    printf("  code at %06X:", pc);
    for (uint32_t a = pc; a < end; a += 2)
        printf(" %04X", (unsigned)(init_mem[a] << 8 | init_mem[a + 1]));
    printf("\n");
}

static bool compare(machine *x, machine *y, relevo_status sx,
                    relevo_status sy, bool full_mem)
{
    relevo_regs *a = relevo_get_regs(x->cpu), *b = relevo_get_regs(y->cpu);
    bool ok = true;
#define CMP(field, name)                                                  \
    if (a->field != b->field) {                                           \
        printf("  %s: interp %08X jit %08X\n", name, (unsigned)a->field,  \
               (unsigned)b->field);                                       \
        ok = false;                                                       \
    }
    static const char *const dn[8] = { "D0", "D1", "D2", "D3", "D4", "D5",
                                       "D6", "D7" };
    static const char *const an[8] = { "A0", "A1", "A2", "A3", "A4", "A5",
                                       "A6", "A7" };
    for (int i = 0; i < 8; i++) {
        CMP(d[i], dn[i]);
        CMP(a[i], an[i]);
    }
    CMP(pc, "PC");
    CMP(sr, "SR");
    CMP(usp, "USP");
    CMP(isp, "ISP");
    CMP(msp, "MSP");
#undef CMP
    if (sx != sy) {
        printf("  status: interp %d jit %d\n", (int)sx, (int)sy);
        ok = false;
    }
    if (relevo_instructions(x->cpu) != relevo_instructions(y->cpu)) {
        printf("  instructions: interp %" PRIu64 " jit %" PRIu64 "\n",
               relevo_instructions(x->cpu), relevo_instructions(y->cpu));
        ok = false;
    }
    if (x->nlog != y->nlog
        || memcmp(x->log, y->log, x->nlog * sizeof x->log[0]) != 0) {
        printf("  host accesses differ (%u / %u)\n", x->nlog, y->nlog);
        ok = false;
    }
    static const uint32_t win[][2] = {
        { 0, 0x8000 }, { CODE, CODE_END }, { WIN - 0x10000, WIN + 0x10000 },
    };
    for (unsigned w = 0; w < 3; w++) {
        uint32_t lo = full_mem ? 0 : win[w][0];
        uint32_t hi = full_mem ? MEM_SIZE : win[w][1];
        if (memcmp(x->mem + lo, y->mem + lo, hi - lo) != 0) {
            for (uint32_t i = lo; i < hi; i++)
                if (x->mem[i] != y->mem[i]) {
                    printf("  memory %06X: interp %02X jit %02X\n", i,
                           x->mem[i], y->mem[i]);
                    break;
                }
            ok = false;
            break;
        }
        if (full_mem)
            break;
    }
    return ok;
}

/* Sets both memories back to init_mem where a case may have changed it. */
static void restore(machine *m, bool full)
{
    if (full) {
        memcpy(m->mem, init_mem, MEM_SIZE);
        relevo_invalidate(m->cpu, 0, MEM_SIZE);
        return;
    }
    memcpy(m->mem, init_mem, 0x8000);
    memcpy(m->mem + CODE, init_mem + CODE, CODE_END - CODE);
    memcpy(m->mem + WIN - 0x10000, init_mem + WIN - 0x10000, 0x20000);
    /* The host wrote the memory: translated code there is stale. */
    relevo_invalidate(m->cpu, 0, 0x8000);
    relevo_invalidate(m->cpu, CODE, CODE_END - CODE);
    relevo_invalidate(m->cpu, WIN - 0x10000, 0x20000);
}

static bool run_case(machine *x, machine *y, unsigned maxlen,
                     bool full_mem)
{
    /* Data and code. */
    for (uint32_t a = WIN - 0x100; a < WIN + WIN_SIZE + 0x100; a += 4)
        put32(init_mem, a, chance(30) ? rnd_addr() : rnd_value());
    unsigned len = 1 + rn(maxlen);
    uint32_t start = CODE + (chance(50) ? 0 : (rn(0x800) & ~1u));
    uint32_t pc = start;
    for (unsigned i = 0; i < len; i++) {
        rnd_insn(pc);
        for (unsigned k = 0; k < nwords && pc + 2 <= CODE_END; k++, pc += 2)
            put16(init_mem, pc, words[k]);
    }
    /* What follows: random words, then NOPs */
    for (uint32_t a = pc; a < pc + 16 && a + 2 <= CODE_END; a += 2)
        put16(init_mem, a, chance(50) ? 0x4E71 : rnd());

    state s;
    for (int i = 0; i < 8; i++) {
        s.d[i] = rnd_value();
        s.a[i] = chance(85) ? rnd_addr() : rnd_value();
    }
    s.sr = (uint16_t)(0x2700 | (rnd() & 0x1F));
    if (chance(20))
        s.sr &= (uint16_t)~0x2000;          /* user mode */
    if (chance(3))
        s.sr |= 0x8000;                     /* trace */
    s.a[7] = WIN + (rn(WIN_SIZE) & ~1u);
    if (chance(5))
        s.a[7] = rnd_addr();
    s.usp = WIN + (rn(WIN_SIZE) & ~1u);
    s.isp = WIN + (rn(WIN_SIZE) & ~1u);
    s.msp = WIN + (rn(WIN_SIZE) & ~1u);
    s.pc = start;

    restore(x, full_mem);
    restore(y, full_mem);
    load_state(x, &s);
    load_state(y, &s);
    uint64_t budget = len + (chance(30) ? rn(20) : 0);
    relevo_status sx = relevo_run(x->cpu, budget);
    relevo_status sy = relevo_run(y->cpu, budget);
    cases++;
    if (compare(x, y, sx, sy, full_mem))
        return true;
    printf("case %lu differs (budget %" PRIu64 ", SR %04X):\n", cases,
           budget, s.sr);
    dump_code(start, pc + 2 > CODE_END ? CODE_END : pc);
    for (int i = 0; i < 8; i++)
        printf("  D%d=%08X A%d=%08X\n", i, s.d[i], i, s.a[i]);
    return false;
}

int main(int argc, char **argv)
{
    unsigned long n = 20000;
    unsigned maxlen = 8;
    bool no_movbe = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            long m = strtol(argv[++i], NULL, 10);
            model = m == 68010 ? RELEVO_68010 : m == 68020 ? RELEVO_68020
                  : m == 68030 ? RELEVO_68030 : m == 68040 ? RELEVO_68040
                  : RELEVO_68000;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            maxlen = (unsigned)strtoul(argv[++i], NULL, 10);
            if (maxlen == 0)
                maxlen = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            rng_state = strtoull(argv[++i], NULL, 0) | 1;
        } else if (strcmp(argv[i], "-x") == 0) {
            no_movbe = true;
        } else {
            fprintf(stderr, "usage: jitdiff [-m MODEL] [-n CASES] [-k MAXLEN]"
                            " [-s SEED] [-x]\n");
            return 2;
        }
    }
    setup_static();
    machine x, y;
    if (!machine_init(&x, RELEVO_ENGINE_INTERP)) {
        fprintf(stderr, "jitdiff: cannot create the CPUs\n");
        return 2;
    }
    if (!machine_init(&y, RELEVO_ENGINE_JIT)) {
        printf("jitdiff: no JIT on this machine: skipped\n");
        return 0;
    }
    if (no_movbe)
        jit_set_movbe(y.cpu, false);
    restore(&x, true);
    restore(&y, true);
    for (unsigned long i = 0; i < n; i++)
        if (!run_case(&x, &y, maxlen, (i & 255) == 255))
            return 1;
    jit_stats js;
    jit_get_stats(y.cpu, &js);
    printf("jitdiff 680%02d%s: %lu cases of up to %u instructions, all equal;"
           " %" PRIu64 " blocks (%" PRIu64 " instructions) translated, %"
           PRIu64 " run, %" PRIu64 " left for the interpreter, %" PRIu64
           " instructions interpreted\n",
           model == RELEVO_68000 ? 0 : model == RELEVO_68010 ? 10
           : model == RELEVO_68020 ? 20 : model == RELEVO_68030 ? 30 : 40,
           no_movbe ? " (BSWAP)" : "", cases, maxlen, js.blocks, js.insns,
           js.runs, js.bails, js.interp);
    relevo_destroy(x.cpu);
    relevo_destroy(y.cpu);
    free(x.mem);
    free(y.mem);
    return 0;
}
