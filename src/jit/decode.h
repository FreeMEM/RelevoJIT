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
    JK_ALU,         /* dst = dst op src (CMP: no write) */
    JK_ADDA,        /* An = An op src (.W sign-extended); CMPA: flags */
    JK_TST,
    JK_CLR,
    JK_BCC,         /* Bcc and BRA (cc 0) */
    JK_BSR,
    JK_DBCC,
    JK_JMP,
    JK_JSR,
    JK_RTS
};

enum jit_alu { JA_ADD, JA_SUB, JA_CMP, JA_AND, JA_OR, JA_EOR };

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
    /* MEM: displacement; the whole address if there is neither base nor
     * index. IMM: the value. */
    uint32_t disp;
} jit_ea;

static inline bool jea_mem_const(const jit_ea *e)
{
    return e->kind == JEA_MEM && e->base < 0 && e->index < 0;
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
    uint8_t size;           /* operation size: 1, 2, 4 */
    uint8_t cc;             /* JK_BCC, JK_DBCC */
    uint8_t flags_def;      /* CCR bits it sets */
    uint8_t flags_use;      /* CCR bits it reads */
    bool may_bail;          /* has a check that can send it to the interp. */
    bool ends_block;
    uint16_t regs;          /* registers it uses */
    uint16_t cycles;        /* rough cycle count */
    jit_ea src, dst;
    uint32_t target;        /* JK_BCC, JK_BSR, JK_DBCC */
} jit_insn;

/* Decodes the instruction at `pc` (even, in a RAM or ROM page). Returns
 * false if the JIT does not translate it there. `*rom` is cleared if any
 * of its words is in RAM. */
bool jit_decode(relevo_cpu *cpu, uint32_t pc, jit_insn *in, bool *rom);

#endif
