/* SPDX-License-Identifier: MPL-2.0
 *
 * The x86-64 code generator.
 *
 * Inside a block:
 *
 *   RBX  the relevo_cpu (registers, page table, counters)
 *   R15  the base of the 68k memory: address a is at [R15 + a]
 *   R14  SR (the CCR bits change, the rest is only stored back)
 *   RSI RDI RBP R8-R13  68k registers, given out per block in order of use
 *   RAX RCX RDX  scratch
 *
 * Every 68k register the block uses is loaded on entry; every one it
 * changes is stored at each exit. Each instruction is emitted in two
 * phases: first its addresses and their checks (alignment on the 68000
 * and 68010, the page kind, not crossing a page), which may leave the block
 * through a "bail" exit with nothing of the instruction done; then its
 * effects. The x86 flags of an operation become N, Z, V and C (SF, ZF,
 * OF and CF; for subtraction x86's CF is the borrow, as on the 68k), and X
 * is C where the 68k sets it. They are stored into R14 only when something
 * reads them later: an instruction of the block, the block's exit, or an
 * instruction that may bail.
 *
 * Blocks never call C. They return to the dispatcher through the exit
 * stub, with EAX 0 (done, regs.pc is the next PC) or 1 (bail: regs.pc is
 * the instruction for the interpreter). A block that ends with a branch to
 * its own start loops inside the block while the budget lasts and nothing
 * asks for attention (an interrupt level change, a stop request).
 */
#if defined(__x86_64__) || defined(_M_X64)

#include <stddef.h>
#include <string.h>

#include "../jit_internal.h"
#include "../../interp/interp.h"
#include "emit.h"

#if defined(__GNUC__)
#include <cpuid.h>
#endif

#define RCPU RBX
#define RMEM R15
#define RSR  R14

static const int host_pool[JIT_HOST_REGS] = {
    RSI, RDI, RBP, R8, R9, R10, R11, R12, R13
};

#define OFF_REG(n) ((int32_t)((n) < 8                                       \
    ? offsetof(relevo_cpu, r.d) + 4u * (unsigned)(n)                         \
    : offsetof(relevo_cpu, r.a) + 4u * (unsigned)((n) - 8)))
#define OFF_PC     ((int32_t)offsetof(relevo_cpu, r.pc))
#define OFF_SR     ((int32_t)offsetof(relevo_cpu, r.sr))
#define OFF_PAGES  ((int32_t)offsetof(relevo_cpu, pages))
#define OFF_COUNT  ((int32_t)offsetof(relevo_cpu, count))
#define OFF_BUDGET ((int32_t)offsetof(relevo_cpu, budget))
#define OFF_CYCLES ((int32_t)offsetof(relevo_cpu, cycles))
#define OFF_ATTN   ((int32_t)offsetof(relevo_cpu, attention))
#define OFF_IR     ((int32_t)offsetof(relevo_cpu, ir))

#define MAX_LABELS (4 * JIT_MAX_INSNS + 8)
#define MAX_FIXUPS (16 * JIT_MAX_INSNS + 16)

typedef struct tr {
    relevo_cpu *c;
    jit_state *j;
    xbuf b;
    int64_t exit_pos;           /* the exit stub, relative to b.p */
    const jit_insn *in;
    unsigned n;
    int host[16];               /* 68k register -> x86 register, or -1 */
    uint16_t used, written;
    int32_t delta[16];          /* (An)+/-(An) not applied yet */
    uint8_t live_out[JIT_MAX_INSNS];
    unsigned cyc_before[JIT_MAX_INSNS + 1];
    int bail[JIT_MAX_INSNS];    /* label of each instruction's bail exit */
    int tail, loop;             /* labels */
    bool movbe;
    uint32_t code_start, code_len;  /* the block's 68k code, masked */
    int64_t label_pos[MAX_LABELS];
    unsigned nlabels;
    struct { size_t at; int label; } fix[MAX_FIXUPS];
    unsigned nfix;
    bool failed;
} tr;

/* ---- Labels ----------------------------------------------------------- */

static int new_label(tr *t)
{
    if (t->nlabels >= MAX_LABELS) {
        t->failed = true;
        return 0;
    }
    t->label_pos[t->nlabels] = -1;
    return (int)t->nlabels++;
}

static void bind(tr *t, int l)
{
    t->label_pos[l] = (int64_t)t->b.pos;
}

static void add_fix(tr *t, size_t at, int l)
{
    if (t->nfix >= MAX_FIXUPS) {
        t->failed = true;
        return;
    }
    t->fix[t->nfix].at = at;
    t->fix[t->nfix].label = l;
    t->nfix++;
}

static void jcc(tr *t, int cc, int l)
{
    add_fix(t, x_jcc32(&t->b, cc), l);
}

static void jmp(tr *t, int l)
{
    add_fix(t, x_jmp32(&t->b), l);
}

/* ---- Registers -------------------------------------------------------- */

static int H(const tr *t, int r)
{
    return t->host[r];
}

/* The 68k registers an instruction changes. */
static uint16_t insn_writes(const jit_insn *in)
{
    uint16_t w = 0;
    const jit_ea *s = &in->src, *d = &in->dst;
    if (s->kind == JEA_MEM && s->step != 0)
        w |= (uint16_t)(1u << s->base);
    if (d->kind == JEA_MEM && d->step != 0)
        w |= (uint16_t)(1u << d->base);
    switch (in->kind) {
    case JK_MOVE:
    case JK_MOVEA:
    case JK_MOVEQ:
    case JK_LEA:
    case JK_CLR:
    case JK_DBCC:
        if (d->kind == JEA_DREG || d->kind == JEA_AREG)
            w |= (uint16_t)(1u << d->reg);
        break;
    case JK_ALU:
    case JK_ADDA:
        if (in->alu != JA_CMP
            && (d->kind == JEA_DREG || d->kind == JEA_AREG))
            w |= (uint16_t)(1u << d->reg);
        break;
    case JK_BSR:
    case JK_JSR:
    case JK_RTS:
        w |= 1u << JR_A(7);
        break;
    default:
        break;
    }
    return w;
}

static void apply_deltas(tr *t)
{
    for (int r = 0; r < 16; r++) {
        if (t->delta[r] != 0) {
            x_lea(&t->b, H(t, r), xm(H(t, r), t->delta[r]));
            t->delta[r] = 0;
        }
    }
}

/* ---- Exits ------------------------------------------------------------ */

/* Adds what instructions [0, k) did to the counters. */
static void count(tr *t, unsigned k)
{
    if (k == 0)
        return;
    x_alu_mi(&t->b, XA_ADD, 8, xm(RCPU, OFF_COUNT), (int32_t)k);
    x_alu_mi(&t->b, XA_SUB, 8, xm(RCPU, OFF_BUDGET), (int32_t)k);
    x_alu_mi(&t->b, XA_ADD, 8, xm(RCPU, OFF_CYCLES),
             (int32_t)t->cyc_before[k]);
}

/* Leaves the block with PC = `pc` after all its instructions. */
static void exit_const(tr *t, uint32_t pc)
{
    x_mov_ri(&t->b, RAX, pc);
    count(t, t->n);
    x_mov_ri(&t->b, RCX, 0);
    jmp(t, t->tail);
}

/* The same with the PC in a register. */
static void exit_reg(tr *t, int r)
{
    if (r != RAX)
        x_mov_rr(&t->b, 4, RAX, r);
    count(t, t->n);
    x_mov_ri(&t->b, RCX, 0);
    jmp(t, t->tail);
}

/* A branch taken to `target`: back to the top of the block if that is
 * where it starts, while the budget lasts and nothing needs attention. */
static void exit_branch(tr *t, uint32_t target)
{
    if (target != t->in[0].pc) {
        exit_const(t, target);
        return;
    }
    xbuf *b = &t->b;
    int out = new_label(t);
    count(t, t->n);
    x_alu_mi(b, XA_CMP, 8, xm(RCPU, OFF_BUDGET), (int32_t)t->n);
    jcc(t, XC_B, out);
    x_alu_mi(b, XA_CMP, 4, xm(RCPU, OFF_ATTN), 0);
    jcc(t, XC_NE, out);
    jmp(t, t->loop);
    bind(t, out);
    x_mov_ri(b, RAX, target);
    x_mov_ri(b, RCX, 0);
    jmp(t, t->tail);
}

/* ---- Flags ------------------------------------------------------------ */

/* Puts the CCR bits in `mask` from the x86 flags of the last operation.
 * `logic`: V and C are known to be 0 (x86 logical operations clear OF and
 * CF too). */
static void flags_x86(tr *t, unsigned mask, bool logic)
{
    xbuf *b = &t->b;
    if (mask == 0)
        return;
    x_pushfq(b);
    x_pop(b, RAX);
    if (logic) {
        /* SF (bit 7) and ZF (bit 6) to N (3) and Z (2) */
        x_shift_ri(b, 5, RAX, 4);
        x_alu_ri(b, XA_AND, 4, RAX, mask & (JF_N | JF_Z));
    } else {
        x_mov_rr(b, 4, RCX, RAX);
        x_shift_ri(b, 5, RCX, 4);
        x_alu_ri(b, XA_AND, 4, RCX, JF_N | JF_Z);
        x_mov_rr(b, 4, RDX, RAX);
        x_alu_ri(b, XA_AND, 4, RDX, 1);                 /* CF -> C */
        x_alu_rr(b, XA_OR, 4, RCX, RDX);
        x_shift_ri(b, 5, RAX, 10);                      /* OF -> V */
        x_alu_ri(b, XA_AND, 4, RAX, JF_V);
        x_alu_rr(b, XA_OR, 4, RAX, RCX);
        if (mask & JF_X) {
            x_shift_ri(b, 4, RDX, 4);                   /* C -> X */
            x_alu_rr(b, XA_OR, 4, RAX, RDX);
        }
        if ((mask & JF_ALL) != JF_ALL)
            x_alu_ri(b, XA_AND, 4, RAX, mask);
    }
    x_alu_ri(b, XA_AND, 4, RSR, ~mask);
    x_alu_rr(b, XA_OR, 4, RSR, RAX);
}

/* The CCR bits in `mask` from constants. */
static void flags_const(tr *t, unsigned mask, unsigned bits)
{
    if (mask == 0)
        return;
    x_alu_ri(&t->b, XA_AND, 4, RSR, ~mask);
    if (bits & mask)
        x_alu_ri(&t->b, XA_OR, 4, RSR, bits & mask);
}

static unsigned nz_of(uint32_t v, unsigned size)
{
    v &= sz_mask(size);
    return (v == 0 ? JF_Z : 0) | ((v & sz_msb(size)) ? JF_N : 0);
}

/* CF = condition `cc` holds for the CCR in R14. */
static void cond_to_cf(tr *t, unsigned cc)
{
    uint32_t table = 0;
    for (unsigned f = 0; f < 16; f++)
        if (cond_true((uint16_t)f, cc))
            table |= 1u << f;
    x_mov_rr(&t->b, 4, RAX, RSR);
    x_alu_ri(&t->b, XA_AND, 4, RAX, 15);
    x_mov_ri(&t->b, RCX, table);
    x_bt_rr(&t->b, RCX, RAX);
}

/* ---- Memory ----------------------------------------------------------- */

/* The address of a memory operand, not masked, into `dst` (RAX or RDX).
 * (An)+ and -(An) are counted in t->delta, applied after the reads. */
static void ea_address(tr *t, const jit_ea *e, int dst)
{
    xbuf *b = &t->b;
    if (jea_mem_const(e)) {
        x_mov_ri(b, dst, e->disp);
        return;
    }
    int32_t disp = (int32_t)e->disp;
    int base = -1, index = -1;
    if (e->base >= 0) {
        if (e->step < 0)
            t->delta[e->base] += e->step;
        disp += t->delta[e->base];
        base = H(t, e->base);
    }
    if (e->index >= 0) {
        int32_t xd = t->delta[e->index];
        if (e->index_long && xd == 0) {
            index = H(t, e->index);
        } else {
            x_lea(b, dst, xm(H(t, e->index), xd));
            if (!e->index_long)
                x_movsx_rr(b, 2, dst, dst);
            index = dst;
        }
    }
    x_lea(b, dst, xmi(base, index, e->scale, disp));
    if (e->base >= 0 && e->step > 0)
        t->delta[e->base] += e->step;
}

/* Checks that `size` bytes at the address in `addr` are a plain access
 * (else bails), and masks the address. */
static void mem_check(tr *t, int addr, unsigned size, bool write, int bail)
{
    xbuf *b = &t->b;
    relevo_cpu *c = t->c;
    if (size > 1 && c->align_check) {
        x_test_ri(b, 1, addr, 1);
        jcc(t, XC_NE, bail);
    }
    if (c->addr_mask != 0xFFFFFFFFu)
        x_alu_ri(b, XA_AND, 4, addr, c->addr_mask);
    x_mov_rr(b, 4, RCX, addr);
    x_shift_ri(b, 5, RCX, RELEVO_PAGE_SHIFT);
    xmem page = xmi(RCPU, RCX, 0, OFF_PAGES);
    if (write) {
        x_alu_mi(b, XA_CMP, 1, page, RELEVO_PAGE_RAM);
        jcc(t, XC_NE, bail);
    } else {
        /* RAM (1) or ROM (2) */
        x_movzx_load(b, 1, RCX, page);
        x_alu_ri(b, XA_SUB, 4, RCX, RELEVO_PAGE_RAM);
        x_alu_ri(b, XA_CMP, 4, RCX, RELEVO_PAGE_ROM - RELEVO_PAGE_RAM);
        jcc(t, XC_A, bail);
    }
    if (size > 1) {
        x_movzx_rr(b, 2, RCX, addr);
        x_alu_ri(b, XA_CMP, 4, RCX, RELEVO_PAGE_SIZE - size);
        jcc(t, XC_A, bail);
    }
    if (write) {
        /* A write into the block's own code: the interpreter does it, and
         * the block is translated again before it runs next. */
        uint32_t lo = t->code_start - (size - 1);
        x_lea(b, RCX, xm(addr, (int32_t)(0u - lo)));
        x_alu_ri(b, XA_CMP, 4, RCX, t->code_len + size - 1);
        jcc(t, XC_B, bail);
    }
}

/* Whether a write at a constant address touches the block's code. */
static bool hits_code(const tr *t, uint32_t addr, unsigned size)
{
    uint64_t a = addr & t->c->addr_mask;
    return a < (uint64_t)t->code_start + t->code_len
        && a + size > t->code_start;
}

/* The masked address of a memory operand into `dst`, checked. */
static void mem_address(tr *t, const jit_ea *e, int dst, unsigned size,
                        bool write, int bail)
{
    if (jea_mem_const(e)) {
        /* checked when decoded */
        if (write && hits_code(t, e->disp, size))
            jmp(t, bail);
        x_mov_ri(&t->b, dst, e->disp & t->c->addr_mask);
        return;
    }
    ea_address(t, e, dst);
    mem_check(t, dst, size, write, bail);
}

static xmem at68k(int addr)
{
    return xmi(RMEM, addr, 0, 0);
}

/* Reads `size` bytes (big-endian) at the masked address in `addr` into
 * `dst`, zero-extended. */
static void load_be(tr *t, unsigned size, int dst, int addr)
{
    xbuf *b = &t->b;
    xmem m = at68k(addr);
    switch (size) {
    case 1:
        x_movzx_load(b, 1, dst, m);
        break;
    case 2:
        if (t->movbe) {
            x_movbe_load(b, 2, dst, m);
            x_movzx_rr(b, 2, dst, dst);
        } else {
            x_movzx_load(b, 2, dst, m);
            x_rol16_8(b, dst);
        }
        break;
    default:
        if (t->movbe) {
            x_movbe_load(b, 4, dst, m);
        } else {
            x_load(b, 4, dst, m);
            x_bswap32(b, dst);
        }
        break;
    }
}

static uint32_t bswap_sz(uint32_t v, unsigned size)
{
    if (size == 1)
        return v & 0xFF;
    if (size == 2)
        return (v >> 8 & 0xFF) | (v & 0xFF) << 8;
    return v >> 24 | (v >> 8 & 0xFF00) | (v & 0xFF00) << 8 | v << 24;
}

/* A value: an x86 register or a constant. */
typedef struct val {
    int reg;            /* -1: constant */
    uint32_t imm;
} val;

/* Writes a value big-endian at the masked address in `addr`, without
 * changing the x86 flags. May use RCX. */
static void store_be(tr *t, unsigned size, int addr, val v)
{
    xbuf *b = &t->b;
    xmem m = at68k(addr);
    if (v.reg < 0) {
        x_store_imm(b, (int)size, m, bswap_sz(v.imm, size));
        return;
    }
    if (size == 1) {
        x_store(b, 1, m, v.reg);
    } else if (t->movbe) {
        x_movbe_store(b, (int)size, m, v.reg);
    } else {
        if (v.reg != RCX)
            x_mov_rr(b, 4, RCX, v.reg);
        if (size == 2)
            x_xchg_cl_ch(b);
        else
            x_bswap32(b, RCX);
        x_store(b, (int)size, m, RCX);
    }
}

/* The source operand of an instruction, after its address checks: a
 * register (copied if (An)+/-(An) is about to change it), memory read into
 * RCX from the address in `addr`, or a constant. */
static val src_value(tr *t, const jit_ea *e, unsigned size, int addr)
{
    val v = { -1, 0 };
    switch (e->kind) {
    case JEA_DREG:
    case JEA_AREG:
        if (t->delta[e->reg] != 0) {
            x_mov_rr(&t->b, 4, RCX, H(t, e->reg));
            v.reg = RCX;
        } else {
            v.reg = H(t, e->reg);
        }
        return v;
    case JEA_MEM:
        load_be(t, size, RCX, addr);
        v.reg = RCX;
        return v;
    default:
        v.imm = e->disp;
        return v;
    }
}

/* ---- Instructions ----------------------------------------------------- */

static int x86_alu(unsigned alu)
{
    switch (alu) {
    case JA_ADD: return XA_ADD;
    case JA_SUB: return XA_SUB;
    case JA_CMP: return XA_CMP;
    case JA_AND: return XA_AND;
    case JA_OR:  return XA_OR;
    default:     return XA_XOR;
    }
}

static bool alu_logic(unsigned alu)
{
    return alu == JA_AND || alu == JA_OR || alu == JA_EOR;
}

static void op_alu_ri_or_rr(tr *t, int op, unsigned size, int dst, val v)
{
    if (v.reg < 0)
        x_alu_ri(&t->b, op, (int)size, dst, v.imm);
    else
        x_alu_rr(&t->b, op, (int)size, dst, v.reg);
}

static void emit_move(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    xbuf *b = &t->b;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    if (in->dst.kind == JEA_MEM)
        mem_address(t, &in->dst, RDX, size, true, bail);
    val v = src_value(t, &in->src, size, RAX);
    apply_deltas(t);
    if (in->dst.kind == JEA_DREG) {
        int d = H(t, in->dst.reg);
        if (v.reg < 0) {
            x_mov_ri_sz(b, (int)size, d, v.imm);
            flags_const(t, fl, nz_of(v.imm, size));
        } else {
            x_mov_rr(b, (int)size, d, v.reg);
            if (fl) {
                x_test_rr(b, (int)size, d, d);
                flags_x86(t, fl, true);
            }
        }
        return;
    }
    if (v.reg >= 0 && fl)
        x_test_rr(b, (int)size, v.reg, v.reg);
    store_be(t, size, RDX, v);
    if (v.reg < 0)
        flags_const(t, fl, nz_of(v.imm, size));
    else
        flags_x86(t, fl, true);
}

static void emit_movea(tr *t, const jit_insn *in, int bail)
{
    unsigned size = in->size;
    xbuf *b = &t->b;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    apply_deltas(t);
    int d = H(t, in->dst.reg);
    if (v.reg < 0)
        x_mov_ri(b, d, size == 2 ? sext(v.imm, SZ_W) : v.imm);
    else if (size == 2)
        x_movsx_rr(b, 2, d, v.reg);
    else
        x_mov_rr(b, 4, d, v.reg);
}

/* ADDA, SUBA, CMPA, and ADDQ/SUBQ to An. */
static void emit_adda(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    xbuf *b = &t->b;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    if (v.reg < 0) {
        if (size == 2)
            v.imm = sext(v.imm, SZ_W);
    } else if (size == 2) {
        x_movsx_rr(b, 2, RCX, v.reg);
        v.reg = RCX;
    }
    apply_deltas(t);
    int d = H(t, in->dst.reg);
    op_alu_ri_or_rr(t, x86_alu(in->alu), 4, d, v);
    if (in->alu == JA_CMP)
        flags_x86(t, fl, false);
}

static void emit_alu(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    int op = x86_alu(in->alu);
    bool logic = alu_logic(in->alu);
    if (in->dst.kind == JEA_DREG) {
        if (in->src.kind == JEA_MEM)
            mem_address(t, &in->src, RAX, size, false, bail);
        val v = src_value(t, &in->src, size, RAX);
        apply_deltas(t);
        op_alu_ri_or_rr(t, op, size, H(t, in->dst.reg), v);
        flags_x86(t, fl, logic);
        return;
    }
    /* to memory: read, change, write */
    mem_address(t, &in->dst, RDX, size, in->alu != JA_CMP, bail);
    load_be(t, size, RCX, RDX);
    val v = src_value(t, &in->src, size, -1);
    apply_deltas(t);
    op_alu_ri_or_rr(t, op, size, RCX, v);
    if (in->alu != JA_CMP) {
        val r = { RCX, 0 };
        store_be(t, size, RDX, r);
    }
    flags_x86(t, fl, logic);
}

static void emit_tst(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    apply_deltas(t);
    if (fl) {
        x_test_rr(&t->b, (int)size, v.reg, v.reg);
        flags_x86(t, fl, true);
    }
}

static void emit_clr(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    if (in->dst.kind == JEA_MEM) {
        mem_address(t, &in->dst, RDX, size, true, bail);
        apply_deltas(t);
        val zero = { -1, 0 };
        store_be(t, size, RDX, zero);
    } else {
        x_mov_ri_sz(&t->b, (int)size, H(t, in->dst.reg), 0);
    }
    flags_const(t, fl, JF_Z);
}

static void emit_bcc(tr *t, const jit_insn *in)
{
    uint32_t next = in->pc + in->len;
    if (in->cc == 0) {
        exit_branch(t, in->target);
        return;
    }
    int taken = new_label(t);
    cond_to_cf(t, in->cc);
    jcc(t, XC_B, taken);
    exit_const(t, next);
    bind(t, taken);
    exit_branch(t, in->target);
}

static void emit_dbcc(tr *t, const jit_insn *in)
{
    uint32_t next = in->pc + in->len;
    if (in->cc == 0) {          /* DBT: never loops */
        exit_const(t, next);
        return;
    }
    int out = new_label(t);
    if (in->cc != 1) {
        cond_to_cf(t, in->cc);
        jcc(t, XC_B, out);
    }
    /* Dn.W - 1: CF when it was 0, i.e. the counter is now -1 */
    x_alu_ri(&t->b, XA_SUB, 2, H(t, in->dst.reg), 1);
    jcc(t, XC_B, out);
    exit_branch(t, in->target);
    bind(t, out);
    exit_const(t, next);
}

/* BSR and JSR: push the return address (checked: a long at A7 - 4). */
static void push_return(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    int sp = H(t, JR_A(7));
    x_lea(b, RAX, xm(sp, -4));
    mem_check(t, RAX, 4, true, bail);
    val ret = { -1, in->pc + in->len };
    store_be(t, 4, RAX, ret);
    x_lea(b, sp, xm(sp, -4));
}

static void emit_jump(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    switch (in->kind) {
    case JK_BSR:
        push_return(t, in, bail);
        exit_const(t, in->target);
        return;
    case JK_JMP:
    case JK_JSR:
        ea_address(t, &in->src, RDX);
        if (!jea_mem_const(&in->src)) {
            x_test_ri(b, 1, RDX, 1);    /* odd: address error */
            jcc(t, XC_NE, bail);
        }
        if (in->kind == JK_JSR)
            push_return(t, in, bail);
        exit_reg(t, RDX);
        return;
    default: {  /* RTS */
        int sp = H(t, JR_A(7));
        x_mov_rr(b, 4, RAX, sp);
        mem_check(t, RAX, 4, false, bail);
        load_be(t, 4, RDX, RAX);
        x_test_ri(b, 1, RDX, 1);
        jcc(t, XC_NE, bail);
        x_lea(b, sp, xm(sp, 4));
        exit_reg(t, RDX);
        return;
    }
    }
}

static void emit_insn(tr *t, unsigned i)
{
    const jit_insn *in = &t->in[i];
    unsigned fl = in->flags_def & t->live_out[i];
    int bail = -1;
    if (in->may_bail) {
        bail = new_label(t);
        t->bail[i] = bail;
    }
    memset(t->delta, 0, sizeof t->delta);
    switch (in->kind) {
    case JK_NOP:
        break;
    case JK_MOVEQ:
        x_mov_ri(&t->b, H(t, in->dst.reg), in->src.disp);
        flags_const(t, fl, nz_of(in->src.disp, 4));
        break;
    case JK_MOVE:
        emit_move(t, in, fl, bail);
        break;
    case JK_MOVEA:
        emit_movea(t, in, bail);
        break;
    case JK_LEA:
        ea_address(t, &in->src, RAX);
        x_mov_rr(&t->b, 4, H(t, in->dst.reg), RAX);
        break;
    case JK_ADDA:
        emit_adda(t, in, fl, bail);
        break;
    case JK_ALU:
        emit_alu(t, in, fl, bail);
        break;
    case JK_TST:
        emit_tst(t, in, fl, bail);
        break;
    case JK_CLR:
        emit_clr(t, in, fl, bail);
        break;
    case JK_BCC:
        emit_bcc(t, in);
        break;
    case JK_DBCC:
        emit_dbcc(t, in);
        break;
    default:
        emit_jump(t, in, bail);
        break;
    }
}

/* ---- Blocks ----------------------------------------------------------- */

static void store_regs(tr *t)
{
    for (int r = 0; r < 16; r++)
        if (t->written & (1u << r))
            x_store(&t->b, 4, xm(RCPU, OFF_REG(r)), H(t, r));
}

const uint8_t *jit_backend_translate(relevo_cpu *c, jit_state *j,
                                     unsigned n)
{
    tr tt, *t = &tt;
    memset(t, 0, sizeof *t);
    t->c = c;
    t->j = j;
    t->in = j->insns;
    t->n = n;
    t->movbe = j->movbe;
    t->b.p = j->mem.rw + j->used;
    t->b.cap = j->mem.size - j->used;
    t->exit_pos = (int64_t)j->exit_off - (int64_t)j->used;

    /* Registers, by order of first use. */
    for (int r = 0; r < 16; r++)
        t->host[r] = -1;
    unsigned next = 0;
    for (unsigned i = 0; i < n; i++) {
        for (int r = 0; r < 16; r++) {
            if ((t->in[i].regs & (1u << r)) && t->host[r] < 0) {
                if (next >= JIT_HOST_REGS)
                    return NULL;    /* the block former keeps to 9 */
                t->host[r] = host_pool[next++];
            }
        }
        t->used |= t->in[i].regs;
        t->written |= insn_writes(&t->in[i]);
    }

    /* Flags: which ones are read after each instruction (backwards). */
    unsigned live = JF_ALL;
    for (unsigned i = n; i-- > 0;) {
        const jit_insn *in = &t->in[i];
        t->live_out[i] = (uint8_t)live;
        live = (live & ~(unsigned)in->flags_def) | in->flags_use;
        if (in->may_bail)
            live = JF_ALL;
    }
    t->code_start = t->in[0].pc & c->addr_mask;
    t->code_len = t->in[n - 1].pc + t->in[n - 1].len - t->in[0].pc;
    t->cyc_before[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        t->cyc_before[i + 1] = t->cyc_before[i] + t->in[i].cycles;
        t->bail[i] = -1;
    }

    t->tail = new_label(t);
    t->loop = new_label(t);
    xbuf *b = &t->b;

    /* Entry: load the registers and SR. */
    for (int r = 0; r < 16; r++)
        if (t->used & (1u << r))
            x_load(b, 4, H(t, r), xm(RCPU, OFF_REG(r)));
    x_movzx_load(b, 2, RSR, xm(RCPU, OFF_SR));
    bind(t, t->loop);

    for (unsigned i = 0; i < n; i++)
        emit_insn(t, i);
    if (!t->in[n - 1].ends_block)
        exit_const(t, t->in[n - 1].pc + t->in[n - 1].len);

    /* The bail exits: PC of the instruction, what came before counted. */
    int bail_tail = new_label(t);
    for (unsigned i = 0; i < n; i++) {
        if (t->bail[i] < 0)
            continue;
        bind(t, t->bail[i]);
        x_mov_ri(b, RAX, t->in[i].pc);
        count(t, i);
        jmp(t, bail_tail);
    }
    bind(t, bail_tail);
    x_mov_ri(b, RCX, 1);

    /* The common tail: EAX the next PC, ECX the result. IR is the last
     * opcode run, for the 68000's frame if fetching the next one faults
     * (after a bail the interpreter sets it again). */
    bind(t, t->tail);
    x_store(b, 4, xm(RCPU, OFF_PC), RAX);
    x_store_imm(b, 2, xm(RCPU, OFF_IR), t->in[n - 1].op);
    store_regs(t);
    x_store(b, 2, xm(RCPU, OFF_SR), RSR);
    x_mov_rr(b, 4, RAX, RCX);
    add_fix(t, x_jmp32(b), -1);

    if (b->overflow || t->failed)
        return NULL;
    for (unsigned k = 0; k < t->nfix; k++) {
        int l = t->fix[k].label;
        x_link(b, t->fix[k].at, l < 0 ? t->exit_pos : t->label_pos[l]);
    }
    const uint8_t *code = j->mem.rx + j->used;
    j->used += (b->pos + 15) & ~(size_t)15;
    if (j->used > j->mem.size)
        j->used = j->mem.size;
    return code;
}

/* ---- The trampoline --------------------------------------------------- */

/* enter(cpu, mem, code): saves the callee-saved registers, sets RBX and
 * R15 and jumps to the block; the exit stub undoes it and returns EAX. */
bool jit_backend_init(relevo_cpu *c, jit_state *j)
{
    (void)c;
    xbuf bb = { j->mem.rw, 0, j->mem.size, false }, *b = &bb;
    static const int saved[] = {
        RBX, RBP, R12, R13, R14, R15,
#if defined(_WIN32)
        RSI, RDI,
#endif
    };
    const int nsaved = (int)(sizeof saved / sizeof saved[0]);
    for (int i = 0; i < nsaved; i++)
        x_push(b, saved[i]);
    /* keep RSP 16-byte aligned */
    bool pad = (nsaved % 2) == 0;
    if (pad)
        x_rsp_adjust(b, -8);
#if defined(_WIN32)
    x_mov_rr(b, 8, RCPU, RCX);
    x_mov_rr(b, 8, RMEM, RDX);
    x_jmp_r(b, R8);
#else
    x_mov_rr(b, 8, RCPU, RDI);
    x_mov_rr(b, 8, RMEM, RSI);
    x_jmp_r(b, RDX);
#endif
    j->exit_off = (b->pos + 15) & ~(size_t)15;
    while (b->pos < j->exit_off)
        x_b(b, 0xCC);
    if (pad)
        x_rsp_adjust(b, 8);
    for (int i = nsaved; i-- > 0;)
        x_pop(b, saved[i]);
    x_ret(b);
    if (b->overflow)
        return false;
    j->enter = j->mem.rx;
    j->used = j->base_used = (b->pos + 15) & ~(size_t)15;
    return true;
}

typedef int (*enter_fn)(relevo_cpu *, uint8_t *, const uint8_t *);

/* Clang's -fsanitize=function checks a signature stored before the target
 * of an indirect call; generated code has none. */
#if defined(__clang__) && __clang_major__ >= 17
__attribute__((no_sanitize("function")))
#endif
int jit_backend_enter(relevo_cpu *c, jit_state *j, const uint8_t *code)
{
    enter_fn f;
    const uint8_t *p = j->enter;
    memcpy(&f, &p, sizeof f);
    return f(c, c->mem, code);
}

bool jit_backend_available(void)
{
    return true;
}

bool jit_backend_has_movbe(void)
{
#if defined(__GNUC__)
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return false;
    return (c >> 22) & 1;
#else
    return false;
#endif
}

#else   /* not x86-64 */

#include "../jit_internal.h"

bool jit_backend_available(void)
{
    return false;
}

bool jit_backend_has_movbe(void)
{
    return false;
}

bool jit_backend_init(relevo_cpu *c, jit_state *j)
{
    (void)c;
    (void)j;
    return false;
}

const uint8_t *jit_backend_translate(relevo_cpu *c, jit_state *j,
                                     unsigned n)
{
    (void)c;
    (void)j;
    (void)n;
    return NULL;
}

int jit_backend_enter(relevo_cpu *c, jit_state *j, const uint8_t *code)
{
    (void)c;
    (void)j;
    (void)code;
    return 1;
}

#endif
