/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT's 68k decoder: what an instruction does, in a form the code
 * generator uses directly. It only accepts what the generator translates;
 * everything else is left to the interpreter.
 */
#ifndef RELEVO_JIT_DECODE_H
#define RELEVO_JIT_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "../cpu.h"

enum jit_kind {
    JK_NOP,
    JK_MOVE,        /* src -> dst (Dn or memory); N Z, V C cleared */
    JK_MOVEA,       /* src (.W sign-extended) -> An */
    JK_MOVEQ,       /* imm -> Dn */
    JK_LEA,         /* address of src -> An */
    JK_PEA,         /* push the address of src */
    JK_ALU,         /* dst = dst op src (CMP: no write) */
    JK_ADDA,        /* An = An op src (.W sign-extended); CMPA: flags */
    JK_CMPM,        /* CMP (Ay)+,(Ax)+ */
    JK_TST,
    JK_CLR,
    JK_UNARY,       /* NEG, NEGX, NOT on dst (sub: jit_unary) */
    JK_ADDX,        /* ADDX/SUBX src,dst (sub: 0 add, 1 sub) */
    JK_SHIFT,       /* shifts and rotates on Dn by #1-8, or memory by 1 */
    JK_BIT,         /* BTST BCHG BCLR BSET #n or Dn (src) on dst */
    JK_MUL,         /* MULU.W/MULS.W src,Dn (sub: signed) */
    JK_DIV,         /* DIVU.W/DIVS.W src,Dn (sub: signed) */
    JK_MULL,        /* MULx.L (ext: the extension word) */
    JK_DIVL,        /* DIVx.L */
    JK_EXT,         /* EXT.W, EXT.L, EXTB.L (sub: jit_ext) on Dn */
    JK_SWAP,
    JK_EXG,         /* src <-> dst registers */
    JK_SCC,
    JK_MOVEM,       /* mask: the register list; sub: 1 memory to registers */
    JK_LINK,        /* LINK An (dst),#imm (src) */
    JK_UNLK,
    JK_TAS,         /* TAS dst (a byte) */
    JK_CHK,         /* CHK src,Dn (dst), .W or .L */
    JK_CAS,         /* CAS Dc,Du,dst (ext: the extension word) */
    JK_MOVEP,       /* MOVEP between Dn and (d16,Ay); sub 1: to memory */
    JK_CALL,        /* the interpreter's handler, called from the block;
                     * memory it touches is checked first: sub 1, a bit
                     * field (dst, ext; 5 bytes, `write`); sub 2, ABCD or
                     * SBCD -(Ay),-(Ax) (src, dst); sub 3, NBCD in memory
                     * (dst) */
    JK_FPU,         /* an FPU instruction, by fpu_jit_op() (see decode.c):
                     * ext the command word, dst the operand, size its
                     * bytes */
    JK_FBCC,        /* FBcc (cc: the predicate) */
    JK_BCC,         /* Bcc and BRA (cc 0) */
    JK_BSR,
    JK_DBCC,
    JK_JMP,
    JK_JSR,
    JK_RTS
};

enum jit_alu { JA_ADD, JA_SUB, JA_CMP, JA_AND, JA_OR, JA_EOR };
enum jit_unary { JU_NEGX, JU_NEG, JU_NOT };
enum jit_shift { JS_AS, JS_LS, JS_ROX, JS_RO };
enum jit_bit { JB_TST, JB_CHG, JB_CLR, JB_SET };
enum jit_ext { JE_W, JE_L, JE_BL };

enum jit_ea_kind { JEA_NONE, JEA_DREG, JEA_AREG, JEA_MEM, JEA_IMM };

/* Registers are numbered 0-15: D0-D7, then A0-A7. */
#define JR_A(n) ((n) + 8)

typedef struct jit_ea {
    uint8_t kind;       /* jit_ea_kind */
    uint8_t reg;        /* DREG/AREG: 0-15 (as JR_A for An) */
    int8_t base;        /* MEM: register 0-15, or -1 */
    int8_t index;       /* MEM: register 0-15, or -1 */
    uint8_t index_long; /* index is Xn.L (else Xn.W) */
    uint8_t scale;      /* index shift, 0-3 */
    int8_t step;        /* (An)+: +bytes, -(An): -bytes, else 0 */
    /* MEM, 68020 memory indirect: 0 none, 1 the index (if any) is added
     * before the indirection, 2 after it. */
    uint8_t ind;
    /* MEM at a constant address that is not plain but cannot fault (a host
     * page, a ROM write): the code always calls the interpreter. */
    uint8_t host;
    /* MEM: displacement; the whole address if there is neither base nor
     * index. IMM: the value. */
    uint32_t disp;
    uint32_t od;        /* MEM with ind: the outer displacement */
} jit_ea;

/* A memory operand at an address known when translating. */
static inline bool jea_mem_const(const jit_ea *e)
{
    return e->kind == JEA_MEM && e->base < 0 && e->index < 0 && !e->ind;
}

/* CCR bits as in SR. */
#define JF_C 0x01u
#define JF_V 0x02u
#define JF_Z 0x04u
#define JF_N 0x08u
#define JF_X 0x10u
#define JF_NZVC 0x0Fu
#define JF_ALL 0x1Fu

typedef struct jit_insn {
    uint32_t pc;
    uint16_t op;
    uint8_t len;            /* bytes */
    uint8_t kind;           /* jit_kind */
    uint8_t alu;            /* jit_alu (JK_ALU, JK_ADDA) */
    uint8_t sub;            /* the variant, by kind (see jit_kind) */
    uint8_t size;           /* operation size: 1, 2, 4 */
    uint8_t cc;             /* JK_BCC, JK_DBCC, JK_SCC */
    uint8_t flags_def;      /* CCR bits it sets */
    uint8_t flags_use;      /* CCR bits it reads */
    bool may_bail;          /* has a check that can send it to the interp. */
    bool ends_block;
    bool left;              /* JK_SHIFT */
    bool write;             /* JK_CALL with sub 1 */
    uint8_t count;          /* JK_SHIFT: 1-8 */
    uint16_t regs;          /* registers it needs in x86 registers */
    uint16_t wregs;         /* registers it changes (also in memory) */
    uint16_t mask;          /* JK_MOVEM */
    uint16_t ext;           /* JK_MULL, JK_DIVL: the extension word */
    uint16_t cycles;        /* rough cycle count */
    jit_ea src, dst;
    uint32_t target;        /* JK_BCC, JK_BSR, JK_DBCC */
} jit_insn;

/* Decodes the instruction at `pc` (even, in a RAM or ROM page). Returns
 * false if the JIT does not translate it there. `*rom` is cleared if any
 * of its words is in RAM. */
bool jit_decode(relevo_cpu *cpu, uint32_t pc, jit_insn *in, bool *rom);

#endif
