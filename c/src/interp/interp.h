/* SPDX-License-Identifier: MPL-2.0
 *
 * What the interpreter's instruction handlers share: operand sizes, the
 * instruction stream, condition codes, effective addresses, jumps and the
 * opcode table.
 *
 * Adding instructions
 * -------------------
 * Each group of instructions lives in its own src/interp/ops_<group>.c with
 * one entry point, `void ops_<group>_init(op_table *tb)`, that puts its
 * handlers in the table with table_fill(). The groups are listed once, in
 * OPS_GROUPS below; a new file needs one line there, nothing else (the
 * Makefile picks up every .c file). Several files of one group can share
 * a private header.
 *
 * A handler gets the opcode word; regs.pc already points after it. It reads
 * its extension words with fetch16()/fetch32() or through ea_decode(), in
 * the order they come. Memory goes through ea_read()/ea_write() or the
 * cpu_read*()/cpu_write*() accesses of cpu.h, which raise address and bus
 * errors by themselves (the handler simply never returns).
 *
 * Address errors on the 68000: the frame holds the PC the microcode had
 * reached, not the instruction's address. ea_read() and ea_write() stack the
 * value that ea_decode() worked out (ea.ae_pc); handlers that write after
 * the next prefetch (MOVE, MOVEM) set ea.ae_pc themselves. See ea.c.
 *
 * Cycles: bus accesses count themselves (4 per byte or word, every word of
 * the instruction included), ea_decode()/ea_read() add the 2 cycles of the
 * indexed modes and of -(An) on reads, jump() and the exceptions add the
 * prefetches from the new address. A handler adds the rest with cycles():
 * the manual's time for the instruction minus 4 per word and per access.
 * SingleStepTests checks it ("cyc" column of build/sst).
 */
#ifndef RELEVO_INTERP_H
#define RELEVO_INTERP_H

#include "../cpu.h"

/* ---- Sizes ------------------------------------------------------------ */

enum { SZ_B = 1, SZ_W = 2, SZ_L = 4 };

static inline uint32_t sz_mask(unsigned size)
{
    return size == SZ_L ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
}

static inline uint32_t sz_msb(unsigned size)
{
    return 1u << (8 * size - 1);
}

/* Sign extension of the low `size` bytes to 32 bits. */
static inline uint32_t sext(uint32_t v, unsigned size)
{
    uint32_t m = sz_msb(size);
    v &= sz_mask(size);
    return (v ^ m) - m;
}

/* The usual size field (bits 7-6): 00 byte, 01 word, 10 long; 0 for 11. */
static inline unsigned size_field(uint16_t op)
{
    static const unsigned sizes[4] = { SZ_B, SZ_W, SZ_L, 0 };
    return sizes[(op >> 6) & 3];
}

/* Writes the low `size` bytes of a data register. */
static inline void set_dreg(relevo_cpu *c, unsigned reg, uint32_t v,
                            unsigned size)
{
    uint32_t m = sz_mask(size);
    c->r.d[reg] = (c->r.d[reg] & ~m) | (v & m);
}

/* ---- Instruction stream ----------------------------------------------- */

static inline uint32_t fetch16(relevo_cpu *c)
{
    uint32_t v = cpu_read16(c, c->r.pc, ACC_PROG);
    c->r.pc += 2;
    return v;
}

static inline uint32_t fetch32(relevo_cpu *c)
{
    uint32_t v = cpu_read32(c, c->r.pc, ACC_PROG);
    c->r.pc += 4;
    return v;
}

/* ---- Memory by size --------------------------------------------------- */

static inline uint32_t cpu_read(relevo_cpu *c, uint32_t addr, unsigned size,
                                unsigned acc)
{
    switch (size) {
    case SZ_B: return cpu_read8(c, addr, acc);
    case SZ_W: return cpu_read16(c, addr, acc);
    default:   return cpu_read32(c, addr, acc);
    }
}

static inline void cpu_write(relevo_cpu *c, uint32_t addr, uint32_t v,
                             unsigned size)
{
    switch (size) {
    case SZ_B: cpu_write8(c, addr, v); break;
    case SZ_W: cpu_write16(c, addr, v); break;
    default:   cpu_write32(c, addr, v); break;
    }
}

static inline uint32_t cpu_pop16(relevo_cpu *c)
{
    uint32_t v = cpu_read16(c, c->r.a[7], ACC_DATA);
    c->r.a[7] += 2;
    return v;
}

static inline uint32_t cpu_pop32(relevo_cpu *c)
{
    uint32_t v = cpu_read32(c, c->r.a[7], ACC_DATA);
    c->r.a[7] += 4;
    return v;
}

/* ---- Cycles ----------------------------------------------------------- */

/* Internal cycles of an instruction, beyond its bus accesses (which count
 * themselves: 4 per byte or word, every instruction word included). */
static inline void cycles(relevo_cpu *c, unsigned n)
{
    c->cycles += n;
}

/* ---- Condition codes -------------------------------------------------- */

#define CCR_C RELEVO_SR_C
#define CCR_V RELEVO_SR_V
#define CCR_Z RELEVO_SR_Z
#define CCR_N RELEVO_SR_N
#define CCR_X RELEVO_SR_X

/* Replaces the bits in `mask` of the CCR with those of `bits`. */
static inline void set_ccr_bits(relevo_cpu *c, unsigned mask, unsigned bits)
{
    c->r.sr = (uint16_t)((c->r.sr & ~mask) | (bits & mask));
}

/* N and Z from the result; V and C cleared; X kept. The flags of MOVE,
 * CLR, TST, the logical operations... */
static inline void set_nz(relevo_cpu *c, uint32_t v, unsigned size)
{
    unsigned f = 0;
    v &= sz_mask(size);
    if (v == 0)
        f |= CCR_Z;
    if (v & sz_msb(size))
        f |= CCR_N;
    set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
}

/* Condition `cc` (0-15: T F HI LS CC CS NE EQ VC VS PL MI GE LT GT LE). */
bool cond_true(uint16_t sr, unsigned cc);

/* ---- Effective addresses ---------------------------------------------- */

typedef enum ea_kind {
    EA_DREG,        /* addr is the register number */
    EA_AREG,        /* addr is the register number */
    EA_MEM,         /* addr is the address (32 bits, not masked) */
    EA_IMM          /* addr is the value */
} ea_kind;

typedef struct ea {
    uint32_t addr;
    uint32_t ae_pc;     /* stacked PC if an access here faults */
    uint8_t kind;       /* ea_kind */
    uint8_t mode, reg;  /* as in the opcode */
    uint8_t size;
    int8_t step;        /* (An)+ / -(An): register change still to apply */
    uint8_t acc;        /* ACC_DATA, or ACC_PROG for PC relative */
} ea_t;

/* Works out the effective address of mode/reg for an operand of `size`
 * bytes, fetching its extension words. (An)+ and -(An) leave the register
 * as it is: the change is applied by ea_read()/ea_write() (or
 * ea_commit()), at the point where the 68000 does it. The caller must only
 * pass modes valid for the instruction (the table takes care of it). */
void ea_decode(relevo_cpu *c, ea_t *e, unsigned mode, unsigned reg,
               unsigned size);

/* Reads the operand (zero-extended). Address registers give their low
 * `size` bytes. */
uint32_t ea_read(relevo_cpu *c, ea_t *e);

/* Writes the operand. A data register keeps its upper bytes; an address
 * register gets the value sign-extended to 32 bits. Used after ea_read()
 * on the same operand (read-modify-write) it does not move (An)+/-(An)
 * again. */
void ea_write(relevo_cpu *c, ea_t *e, uint32_t v);

/* Applies a pending (An)+/-(An) change. */
void ea_commit(relevo_cpu *c, ea_t *e);

/* The mode/reg pair of the low 6 bits of an opcode. */
static inline void ea_decode_op(relevo_cpu *c, ea_t *e, uint16_t op,
                                unsigned size)
{
    ea_decode(c, e, (op >> 3) & 7, op & 7, size);
}

/* ---- Flow ------------------------------------------------------------- */

/* PC = target. An odd target is an address error when the 68000 prefetches
 * there, still inside the jumping instruction; `ae_pc` is what it stacks.
 * Counts the cycles of refilling the prefetch queue at the target. */
void jump(relevo_cpu *c, uint32_t target, uint32_t ae_pc);

/* Privileged instructions: false (after taking the privilege violation) in
 * user mode. */
static inline bool supervisor_or_trap(relevo_cpu *c)
{
    if (LIKELY(c->r.sr & RELEVO_SR_S))
        return true;
    cpu_exception_group1(c, VEC_PRIVILEGE);
    return false;
}

/* ---- The opcode table ------------------------------------------------- */

/* Effective address classes, as sets of mode/reg slots (M68000PRM 2.2). */
enum {
    EAB_DREG = 1 << 0,
    EAB_AREG = 1 << 1,
    EAB_IND = 1 << 2,       /* (An) */
    EAB_POSTINC = 1 << 3,   /* (An)+ */
    EAB_PREDEC = 1 << 4,    /* -(An) */
    EAB_D16 = 1 << 5,       /* (d16,An) */
    EAB_D8 = 1 << 6,        /* (d8,An,Xn) */
    EAB_ABSW = 1 << 7,      /* (xxx).W */
    EAB_ABSL = 1 << 8,      /* (xxx).L */
    EAB_PCD16 = 1 << 9,     /* (d16,PC) */
    EAB_PCD8 = 1 << 10,     /* (d8,PC,Xn) */
    EAB_IMM = 1 << 11       /* #imm */
};

#define EA_ALL       0x0FFFu
#define EA_DATA      (EA_ALL & ~(unsigned)EAB_AREG)
#define EA_MEMORY    (EA_DATA & ~(unsigned)EAB_DREG)
#define EA_CONTROL   (EAB_IND | EAB_D16 | EAB_D8 | EAB_ABSW | EAB_ABSL \
                      | EAB_PCD16 | EAB_PCD8)
#define EA_ALTERABLE (EAB_DREG | EAB_AREG | EAB_IND | EAB_POSTINC \
                      | EAB_PREDEC | EAB_D16 | EAB_D8 | EAB_ABSW | EAB_ABSL)
#define EA_DATA_ALT  (EA_ALTERABLE & ~(unsigned)EAB_AREG)
#define EA_MEM_ALT   (EA_DATA_ALT & ~(unsigned)EAB_DREG)
#define EA_CONTROL_ALT (EA_CONTROL & ~(unsigned)(EAB_PCD16 | EAB_PCD8))

/* Whether mode/reg belongs to `classes`. */
bool ea_valid(unsigned mode, unsigned reg, unsigned classes);

typedef struct op_table {
    relevo_handler *t;
    relevo_model model;
    relevo_fpu fpu;
    unsigned conflicts;     /* opcodes given a handler twice */
} op_table;

/* Puts `h` at every opcode `base | s`, for every combination s of the bits
 * in `free`, whose low 6 bits are an effective address in `ea_classes`
 * (0: no check). */
void table_fill(op_table *tb, uint16_t base, uint16_t free,
                unsigned ea_classes, relevo_handler h);
/* One opcode. */
void table_set(op_table *tb, uint16_t op, relevo_handler h);

/* The groups, in the order they fill the table. Each file defines
 * ops_<name>_init(). */
#define OPS_GROUPS(X) \
    X(control)        \
    X(move)           \
    X(system)         \
    X(arith)          \
    X(bits)           \
    X(010)            \
    X(020)            \
    X(fpu)

#define OPS_DECLARE(name) void ops_##name##_init(op_table *tb);
OPS_GROUPS(OPS_DECLARE)
#undef OPS_DECLARE

/* The handlers that fill the table before the groups. */
void op_illegal(relevo_cpu *cpu, uint16_t opcode);
void op_line_a(relevo_cpu *cpu, uint16_t opcode);
void op_line_f(relevo_cpu *cpu, uint16_t opcode);

#endif /* RELEVO_INTERP_H */
