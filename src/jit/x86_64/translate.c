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
 * A block starts by checking the budget (enough for all its instructions)
 * and the attention flag (an interrupt level change, a stop request); if
 * either fails it leaves at once. Then it loads every 68k register it uses;
 * every one it changes is stored at each exit.
 *
 * Each instruction is emitted in two phases: first its addresses and their
 * checks, which may leave the block through a "bail" exit with nothing of
 * the instruction done; then its effects. The check of an access looks up
 * the JIT's map of 4 KB pages (jit_internal.h) and the page offset; what
 * the map does not clear at once (a page with translated code, an access
 * across pages) calls jit_access_check() through a shared stub. Alignment
 * on the 68000 and 68010, host pages, missing pages and writes into
 * translated code all bail.
 *
 * Flags: the x86 flags of an operation are N, Z, V and C (SF, ZF, OF and
 * CF; for subtraction x86's CF is the borrow, as on the 68k), and X is C
 * where the 68k sets it. They are kept only when something reads them
 * later (an instruction of the block, the block's exit, or an instruction
 * that may bail), and not at once: they stay "pending" in EFLAGS until
 * some code would change them. So a Bcc, DBcc or Scc right after the
 * instruction that set its flags tests EFLAGS directly.
 *
 * Lazy condition codes (when the CPU has LAHF and SAHF in 64-bit mode):
 * pending flags that must be kept are not turned into CCR bits but saved
 * as they are, with LAHF and SETO, in cpu->jit_ccr (the "record", see
 * cpu.h), together with which CCR bits they give; X has its own byte. A
 * CCR bit then lives in EFLAGS (pending), in R14 (when the block knows R14
 * has it: "own"), or else in the record if the record has it, or else in
 * R14. Saving takes four or five instructions and changes no flag, and
 * SAHF brings the flags back for a Bcc. The record goes from block to
 * block: a block that does not touch the flags leaves it as it is, and
 * one that ends with pending flags overwrites it. Only what reads CCR bits
 * from R14 (ADDX, the handler calls, a condition with mixed sources...)
 * merges it first, through a shared stub, and the dispatcher merges it
 * into SR when a block returns. SR itself stays in R14 from block to
 * block: the trampoline loads it and the common exit stores it.
 *
 * Exits: a branch to a known PC ends in a jump that first leads to the
 * dispatcher (JIT_EXIT_CHAIN) and is then patched to go straight to the
 * block there (jit.c). RTS and jumps through registers look the target up
 * in jit_state.ind. A branch to the block's own start loops inside it
 * while the budget lasts and nothing asks for attention. Everything leaves
 * through the common exit with EAX the PC, ECX the exit code and EDX the
 * chained jump's offset.
 *
 * Instructions too rare or too intricate to translate, but with only
 * register operands and no exception possible (shifts by a register,
 * ABCD, bit fields in Dn...), call their interpreter handler from the
 * block (JK_CALL).
 */
#if defined(__x86_64__) || defined(_M_X64)

#include <stddef.h>
#include <stdlib.h>
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
#define OFF_BUDGET ((int32_t)offsetof(relevo_cpu, budget))
#define OFF_CYCLES ((int32_t)offsetof(relevo_cpu, cycles))
#define OFF_ATTN   ((int32_t)offsetof(relevo_cpu, attention))
#define OFF_IR     ((int32_t)offsetof(relevo_cpu, ir))
#define OFF_JIT    ((int32_t)offsetof(relevo_cpu, jit))
#define OFF_CCR    ((int32_t)offsetof(relevo_cpu, jit_ccr))
#define OFF_FPCR   ((int32_t)offsetof(relevo_cpu, r.fpcr))
#define OFF_FPEND  ((int32_t)offsetof(relevo_cpu, fpu_pending))
#define JOFF_MAP   ((int32_t)offsetof(jit_state, map))
#define JOFF_IND   ((int32_t)offsetof(jit_state, ind))

#define MAX_LABELS (16 * JIT_MAX_INSNS + 16)
#define MAX_FIXUPS (32 * JIT_MAX_INSNS + 32)
#define MAX_DEFER  (4 * JIT_MAX_INSNS)

/* Pending flags: what EFLAGS holds for the CCR. */
enum { PM_NONE, PM_LOGIC, PM_ARITH, PM_ARITHX };

typedef struct deferred {
    int slow, cont, bail, host; /* labels */
    int addr;                   /* the register with the masked address */
    uint32_t size_write;        /* size | write << 16 */
} deferred;

typedef struct pending {
    unsigned mask;
    int mode;
} pending;

typedef struct tr {
    relevo_cpu *c;
    jit_state *j;
    xbuf b;
    size_t base;                /* where b.p is in the code memory */
    const jit_insn *in;
    unsigned n;
    int host[16];               /* 68k register -> x86 register, or -1 */
    uint16_t used, written;
    int32_t delta[16];          /* (An)+/-(An) not applied yet */
    uint8_t live_out[JIT_MAX_INSNS];
    unsigned cyc_before[JIT_MAX_INSNS + 1];
    int bail[JIT_MAX_INSNS];    /* label of each instruction's bail exit */
    int hostp[JIT_MAX_INSNS];   /* ... and of its host path, or -1 */
    uint8_t naccess[JIT_MAX_INSNS]; /* memory checks of each instruction */
    int tail, loop, entry_out;  /* labels */
    bool movbe;
    bool lazy;                  /* the record (see the top) is used */
    pending pend;
    unsigned own;               /* CCR bits R14 holds for sure */
    unsigned rec;               /* CCR bits the record holds for sure */
    unsigned cur;               /* the instruction being emitted */
    uint8_t own_at[JIT_MAX_INSNS];  /* `own` at each bail check */
    int64_t label_pos[MAX_LABELS];
    unsigned nlabels;
    struct { size_t at; int label; } fix[MAX_FIXUPS];
    unsigned nfix;
    deferred defer[MAX_DEFER];
    unsigned ndefer;
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

/* A jump or call to shared code at offset `off` of the code memory. */
static void link_abs(tr *t, size_t at, size_t off)
{
    x_patch32(&t->b, at,
              (uint32_t)((int64_t)off - (int64_t)(t->base + at + 4)));
}

static void jmp_abs(tr *t, size_t off)
{
    link_abs(t, x_jmp32(&t->b), off);
}

static void call_abs(tr *t, size_t off)
{
    link_abs(t, x_call32(&t->b), off);
}

/* ---- Registers -------------------------------------------------------- */

static int H(const tr *t, int r)
{
    return t->host[r];
}

/* The x86 register with 68k register r: its own, or `tmp` loaded from
 * memory if the block does not hold it. */
static int reg_get(tr *t, int r, int tmp)
{
    if (H(t, r) >= 0)
        return H(t, r);
    x_load(&t->b, 4, tmp, xm(RCPU, OFF_REG(r)));
    return tmp;
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

/* ---- Flags ------------------------------------------------------------ */

static void pushf_rax(tr *t)
{
    x_pushfq(&t->b);
    x_pop(&t->b, RAX);
}

/* ORs into R14 the bits of `mask` among C, X, V of the flags in EAX
 * (pushed EFLAGS). Uses RCX. */
static void gather_cvx(tr *t, unsigned mask, bool with_x)
{
    xbuf *b = &t->b;
    if (mask & (JF_C | JF_X)) {
        x_mov_rr(b, 4, RCX, RAX);
        x_alu_ri(b, XA_AND, 4, RCX, 1);
        if (mask & JF_C)
            x_alu_rr(b, XA_OR, 4, RSR, RCX);
        if (with_x && (mask & JF_X)) {
            x_shift_ri(b, 4, RCX, 4);
            x_alu_rr(b, XA_OR, 4, RSR, RCX);
        }
    }
    if (mask & JF_V) {
        x_mov_rr(b, 4, RCX, RAX);
        x_shift_ri(b, 5, RCX, 10);
        x_alu_ri(b, XA_AND, 4, RCX, JF_V);
        x_alu_rr(b, XA_OR, 4, RSR, RCX);
    }
}

/* ORs into R14 N and Z (of `mask`) from the flags in EAX. Uses RCX. */
static void gather_nz(tr *t, unsigned mask)
{
    xbuf *b = &t->b;
    if (mask & (JF_N | JF_Z)) {
        x_mov_rr(b, 4, RCX, RAX);
        x_shift_ri(b, 5, RCX, 4);
        x_alu_ri(b, XA_AND, 4, RCX, mask & (JF_N | JF_Z));
        x_alu_rr(b, XA_OR, 4, RSR, RCX);
    }
}

/* The CCR bits in `mask` from EFLAGS, as an operation of `mode` left
 * them. Uses RAX and RCX. */
static void flags_from_eflags(tr *t, unsigned mask, int mode)
{
    xbuf *b = &t->b;
    if (mask == 0)
        return;
    if (mode == PM_LOGIC) {
        unsigned nz = mask & (JF_N | JF_Z);
        if (nz == JF_Z || nz == JF_N) {
            x_setcc(b, nz == JF_Z ? XC_E : XC_S, RAX);
            x_movzx_rr(b, 1, RAX, RAX);
            x_shift_ri(b, 4, RAX, nz == JF_Z ? 2 : 3);
        } else if (nz) {
            pushf_rax(t);
            x_shift_ri(b, 5, RAX, 4);
            x_alu_ri(b, XA_AND, 4, RAX, JF_N | JF_Z);
        }
        x_alu_ri(b, XA_AND, 4, RSR, ~mask);
        if (nz)
            x_alu_rr(b, XA_OR, 4, RSR, RAX);
        return;
    }
    pushf_rax(t);
    if ((mask & JF_NZVC) == JF_NZVC) {
        /* ECX = N Z V C, built from EAX; then X = C if needed */
        x_mov_rr(b, 4, RCX, RAX);
        x_shift_ri(b, 5, RCX, 4);
        x_alu_ri(b, XA_AND, 4, RCX, JF_N | JF_Z);
        x_btx_ri(b, XB_BT, RAX, 0);
        x_alu_ri(b, XA_ADC, 4, RCX, 0);
        x_shift_ri(b, 5, RAX, 10);
        x_alu_ri(b, XA_AND, 4, RAX, JF_V);
        x_alu_rr(b, XA_OR, 4, RCX, RAX);
        x_alu_ri(b, XA_AND, 4, RSR, ~mask);
        x_alu_rr(b, XA_OR, 4, RSR, RCX);
        if (mode == PM_ARITHX && (mask & JF_X)) {
            x_alu_ri(b, XA_AND, 4, RCX, JF_C);
            x_shift_ri(b, 4, RCX, 4);
            x_alu_rr(b, XA_OR, 4, RSR, RCX);
        }
        return;
    }
    x_alu_ri(b, XA_AND, 4, RSR, ~mask);
    gather_nz(t, mask);
    gather_cvx(t, mask, mode == PM_ARITHX);
}

/* Saves the pending flags in the record, as they are. Changes no flag:
 * EFLAGS still has them. */
static void record_pending(tr *t)
{
    xbuf *b = &t->b;
    unsigned m = t->pend.mask;
    x_lahf(b);
    if (m & JF_NZVC) {
        x_setcc(b, XC_O, RAX);
        x_store(b, 2, xm(RCPU, OFF_CCR), RAX);
        x_store_imm(b, 1, xm(RCPU, OFF_CCR + 2), m & JF_NZVC);
        t->rec = (t->rec & JF_X) | (m & JF_NZVC);
    }
    if (m & JF_X) {
        x_store_ah(b, xm(RCPU, OFF_CCR + 3));
        t->rec |= JF_X;
    }
    t->own &= ~m;
    t->pend.mode = PM_NONE;
    t->pend.mask = 0;
}

/* Whether flags pending now are read from R14 later in the block (by an
 * instruction other than a condition, which can take them back from the
 * record): then they are better turned into CCR bits at once. */
static bool read_from_r14_later(const tr *t)
{
    unsigned left = t->pend.mask;
    for (unsigned i = t->cur; i < t->n && left; i++) {
        const jit_insn *in = &t->in[i];
        bool cond = in->kind == JK_BCC || in->kind == JK_DBCC
                 || in->kind == JK_SCC;
        if ((in->flags_use & left) && !cond)
            return true;
        left &= ~(unsigned)in->flags_def;
    }
    return false;
}

/* Keeps the pending flags before something changes EFLAGS: in the record
 * or in R14. */
static void flags_flush(tr *t)
{
    if (t->pend.mode == PM_NONE)
        return;
    if (t->lazy && !read_from_r14_later(t)) {
        record_pending(t);
        return;
    }
    flags_from_eflags(t, t->pend.mask, t->pend.mode);
    t->own |= t->pend.mask;
    t->rec &= ~t->pend.mask;
    t->pend.mode = PM_NONE;
    t->pend.mask = 0;
}

static void flags_pending(tr *t, unsigned mask, int mode)
{
    t->pend.mask = mask;
    t->pend.mode = mask ? mode : PM_NONE;
}

/* Makes R14 hold the CCR bits in `need` (nothing pending): if the record
 * may have one of them, the stub merges it into R14 (keeping what the
 * block wrote there) and empties it. Uses RAX, RCX and RDX. */
static void flags_need(tr *t, unsigned need)
{
    unsigned miss = need & ~t->own;
    if (miss == 0)
        return;
    xbuf *b = &t->b;
    x_test_mi32(b, xm(RCPU, OFF_CCR),
                (miss & JF_NZVC) << 16 | ((miss & JF_X) ? 0xFF000000u : 0));
    int skip = new_label(t);
    jcc(t, XC_E, skip);
    x_mov_ri(b, RCX, t->own);
    call_abs(t, t->j->ccr_off);
    bind(t, skip);
    t->own |= need;
    t->rec = 0;
}

/* Where a block leaves (or bails): makes R14 and the record agree. The
 * pending flags go to the record; what R14 holds for sure is taken out of
 * the record. Changes no flag while flags are pending. */
static void flags_exit(tr *t)
{
    xbuf *b = &t->b;
    if (!t->lazy) {
        flags_flush(t);
        return;
    }
    if (t->pend.mode != PM_NONE && (JF_NZVC & ~t->pend.mask & ~t->own)) {
        /* (never at an exit: pending flags have all of N Z V C) */
        flags_from_eflags(t, t->pend.mask, t->pend.mode);
        t->own |= t->pend.mask;
        t->pend.mode = PM_NONE;
        t->pend.mask = 0;
    }
    if (t->pend.mode != PM_NONE) {
        unsigned m = t->pend.mask;
        bool x_own = (t->own & JF_X) && !(m & JF_X);
        record_pending(t);
        if (x_own)
            x_store_imm(b, 1, xm(RCPU, OFF_CCR + 3), 0);
        return;
    }
    unsigned nzvc = t->own & JF_NZVC;
    bool x = (t->own & JF_X) != 0;
    if (nzvc == JF_NZVC && x) {
        x_store_imm(b, 2, xm(RCPU, OFF_CCR + 2), 0);
        return;
    }
    if (nzvc == JF_NZVC)
        x_store_imm(b, 1, xm(RCPU, OFF_CCR + 2), 0);
    else if (nzvc != 0)
        x_alu_mi(b, XA_AND, 1, xm(RCPU, OFF_CCR + 2), (int32_t)(~nzvc & 0xFF));
    if (x)
        x_store_imm(b, 1, xm(RCPU, OFF_CCR + 3), 0);
}

/* Before a branch: pending flags go to the record now (it changes no flag),
 * so that every way out finds them there. */
static void flags_settle(tr *t)
{
    if (t->lazy && t->pend.mode != PM_NONE)
        record_pending(t);
}

/* The flag state, for code emitted on two paths. */
typedef struct fstate {
    pending pend;
    unsigned own, rec;
} fstate;

static fstate fs_save(const tr *t)
{
    fstate f = { t->pend, t->own, t->rec };
    return f;
}

static void fs_restore(tr *t, fstate f)
{
    t->pend = f.pend;
    t->own = f.own;
    t->rec = f.rec;
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

enum { FB_CF, FB_DL, FB_ZERO };

/* The CCR bits in `mask`: N and Z from SF and ZF, C from CF, the low bit
 * of DL or 0, V from OF, DL or 0 (only one of them from DL), and X = C if
 * `x_eq_c`. Uses RAX, RCX and RDX. */
static void flags_build(tr *t, unsigned mask, int csrc, int vsrc,
                        bool x_eq_c)
{
    xbuf *b = &t->b;
    if (mask == 0)
        return;
    if (csrc == FB_DL || vsrc == FB_DL)
        x_movzx_rr(b, 1, RDX, RDX);
    pushf_rax(t);
    x_alu_ri(b, XA_AND, 4, RSR, ~mask);
    gather_nz(t, mask);
    if (csrc == FB_CF) {
        gather_cvx(t, mask & (JF_C | (x_eq_c ? JF_X : 0)), true);
    } else if (csrc == FB_DL) {
        if (mask & JF_C)
            x_alu_rr(b, XA_OR, 4, RSR, RDX);
        if (x_eq_c && (mask & JF_X)) {
            x_mov_rr(b, 4, RCX, RDX);
            x_shift_ri(b, 4, RCX, 4);
            x_alu_rr(b, XA_OR, 4, RSR, RCX);
        }
    }
    if (vsrc == FB_CF || vsrc == FB_ZERO) {
        if (vsrc == FB_CF)
            gather_cvx(t, mask & JF_V, false);
    } else if (mask & JF_V) {
        x_lea(b, RCX, xmi(RDX, RDX, 0, 0));
        x_alu_rr(b, XA_OR, 4, RSR, RCX);
    }
}

/* ADDX, SUBX, NEGX: all from EFLAGS with X = C, but Z only cleared. */
static void flags_sticky(tr *t, unsigned mask)
{
    xbuf *b = &t->b;
    if (mask == 0)
        return;
    pushf_rax(t);
    if (mask & JF_Z) {
        /* R14 &= ZF ? ~0 : ~Z */
        x_mov_rr(b, 4, RCX, RAX);
        x_shift_ri(b, 5, RCX, 4);
        x_alu_ri(b, XA_AND, 4, RCX, JF_Z);
        x_alu_ri(b, XA_OR, 4, RCX, ~JF_Z);
        x_alu_rr(b, XA_AND, 4, RSR, RCX);
    }
    unsigned m = mask & ~JF_Z;
    x_alu_ri(b, XA_AND, 4, RSR, ~m);
    gather_nz(t, m);
    gather_cvx(t, m, true);
}

static unsigned nz_of(uint32_t v, unsigned size)
{
    v &= sz_mask(size);
    return (v == 0 ? JF_Z : 0) | ((v & sz_msb(size)) ? JF_N : 0);
}

/* CF = condition `cc` holds for the CCR in R14. Uses RAX and RCX. */
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

/* The x86 condition for 68k condition cc (2-15), which reads the CCR bits
 * `need`: on EFLAGS while flags are pending (they hold N, Z, V and C
 * exactly), on EFLAGS taken back from the record when it has them all
 * (SAHF; ADD AL first sets OF from the byte SETO wrote), else on CF after
 * testing R14 (emit_insn made sure R14 has them). Uses RAX and RCX. */
static int cond(tr *t, unsigned cc, unsigned need)
{
    static const uint8_t x86cc[16] = {
        0, 0, XC_A, XC_BE, XC_AE, XC_B, XC_NE, XC_E,
        XC_NO, XC_O, XC_NS, XC_S, XC_GE, XC_L, XC_G, XC_LE
    };
    if (t->pend.mode != PM_NONE)
        return x86cc[cc & 15];
    if ((need & ~t->own) != 0) {
        xbuf *b = &t->b;
        x_load(b, 2, RAX, xm(RCPU, OFF_CCR));
        x_alu_ri(b, XA_ADD, 1, RAX, 0x7F);
        x_sahf(b);
        return x86cc[cc & 15];
    }
    cond_to_cf(t, cc);
    return XC_B;
}

/* ---- Exits ------------------------------------------------------------ */

/* Adds what instructions [0, k) did to the counters. (The instruction
 * count is the dispatcher's: what the budget went down by.) */
static void count(tr *t, unsigned k)
{
    if (k == 0)
        return;
    x_alu_mi(&t->b, XA_SUB, 8, xm(RCPU, OFF_BUDGET), (int32_t)k);
    x_alu_mi(&t->b, XA_ADD, 8, xm(RCPU, OFF_CYCLES),
             (int32_t)t->cyc_before[k]);
}

static void store_regs(tr *t)
{
    for (int r = 0; r < 16; r++)
        if (t->written & (1u << r))
            x_store(&t->b, 4, xm(RCPU, OFF_REG(r)), H(t, r));
}

/* What every exit after the instructions stores: the registers and, for
 * the 68000's fault frames, IR (the last opcode run). (SR stays in R14:
 * the common exit stores it.) */
static void exit_stores(tr *t)
{
    store_regs(t);
    if (t->c->model == RELEVO_68000)
        x_store_imm(&t->b, 2, xm(RCPU, OFF_IR), t->in[t->n - 1].op);
}

/* Leaves the block for a known PC after all its instructions: through a
 * jump that the dispatcher may chain to the block at `pc`. */
static void exit_chain(tr *t, uint32_t pc)
{
    xbuf *b = &t->b;
    flags_exit(t);
    count(t, t->n);
    exit_stores(t);
    size_t at = x_jmp32(b);         /* rel 0: falls through, until patched */
    x_mov_ri(b, RAX, pc);
    x_mov_ri(b, RCX, JIT_EXIT_CHAIN);
    x_mov_ri(b, RDX, (uint32_t)(t->base + at));
    jmp_abs(t, t->j->exit_off);
}

/* Leaves for the PC in `r` (not RAX or RCX): through the cache of
 * indirect targets, else to the dispatcher. */
static void exit_indirect(tr *t, int r)
{
    xbuf *b = &t->b;
    flags_exit(t);
    count(t, t->n);
    exit_stores(t);
    x_mov_rr(b, 4, RAX, r);
    x_mov_rr(b, 4, RCX, RAX);
    x_alu_ri(b, XA_AND, 4, RCX, (JIT_IND_SIZE - 1) << 1);
    x_shift_ri(b, 4, RCX, 3);
    x_add_rm64(b, RCX, xm(RCPU, OFF_JIT));
    static const uint8_t cmp_mr = 0x39;
    x_rm(b, 4, &cmp_mr, 1, RAX, xm(RCX, JOFF_IND), false);
    int miss = new_label(t);
    jcc(t, XC_NE, miss);
    x_jmp_m(b, xm(RCX, JOFF_IND + (int32_t)offsetof(jit_ind, code)));
    bind(t, miss);
    x_mov_ri(b, RCX, JIT_EXIT_NORMAL);
    jmp_abs(t, t->j->exit_off);
}

/* A branch taken to `target`: back to the top of the block if that is
 * where it starts, while the budget lasts and nothing needs attention. */
static void exit_branch(tr *t, uint32_t target)
{
    if (target != t->in[0].pc) {
        exit_chain(t, target);
        return;
    }
    xbuf *b = &t->b;
    int out = new_label(t);
    flags_exit(t);
    count(t, t->n);
    x_alu_mi(b, XA_CMP, 8, xm(RCPU, OFF_BUDGET), (int32_t)t->n);
    jcc(t, XC_B, out);
    x_alu_mi(b, XA_CMP, 4, xm(RCPU, OFF_ATTN), 0);
    jcc(t, XC_NE, out);
    jmp(t, t->loop);
    bind(t, out);
    x_mov_ri(b, RAX, target);
    x_mov_ri(b, RCX, JIT_EXIT_NORMAL);
    jmp(t, t->tail);
}

/* ---- Memory ----------------------------------------------------------- */

static xmem at68k(int addr, int32_t disp)
{
    return xmi(RMEM, addr, 0, disp);
}

/* Reads `size` bytes (big-endian) at [R15 + addr + disp] into `dst`,
 * zero-extended. */
static void load_be(tr *t, unsigned size, int dst, int addr, int32_t disp)
{
    xbuf *b = &t->b;
    xmem m = at68k(addr, disp);
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

static val vreg(int r)
{
    val v = { r, 0 };
    return v;
}

/* Writes a value big-endian at [R15 + addr + disp], without changing the
 * x86 flags. May use RCX. */
static void store_be(tr *t, unsigned size, int addr, int32_t disp, val v)
{
    xbuf *b = &t->b;
    xmem m = at68k(addr, disp);
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

/* The host path of the instruction being emitted (see emit_host_path). */
static int host_label(tr *t)
{
    if (t->hostp[t->cur] < 0)
        t->hostp[t->cur] = new_label(t);
    return t->hostp[t->cur];
}

static void defer(tr *t, int slow, int cont, int bail, int addr,
                  unsigned size, bool write)
{
    if (t->ndefer >= MAX_DEFER) {
        t->failed = true;
        return;
    }
    deferred *d = &t->defer[t->ndefer++];
    d->host = host_label(t);
    d->slow = slow;
    d->cont = cont;
    d->bail = bail;
    d->addr = addr;
    d->size_write = size | (write ? 1u << 16 : 0);
}

/* Checks that `size` bytes at the address in `addr` are a plain access
 * (else bails), and masks the address. `align`: a word or long access,
 * which faults at an odd address on the 68000 and 68010. Uses RCX. */
static void mem_check_x(tr *t, int addr, unsigned size, bool write, int bail,
                        bool align)
{
    xbuf *b = &t->b;
    relevo_cpu *c = t->c;
    t->naccess[t->cur]++;
    if (align && size > 1 && c->align_check) {
        x_test_ri(b, 1, addr, 1);
        jcc(t, XC_NE, bail);
    }
    if (c->addr_mask != 0xFFFFFFFFu)
        x_alu_ri(b, XA_AND, 4, addr, c->addr_mask);
    int slow = new_label(t), cont = new_label(t);
    x_mov_rr(b, 4, RCX, addr);
    x_shift_ri(b, 5, RCX, JIT_MAP_SHIFT);
    x_add_rm64(b, RCX, xm(RCPU, OFF_JIT));
    x_test_mi8(b, xm(RCX, JOFF_MAP),
               write ? JMAP_NO_WRITE | JMAP_CODE : JMAP_NO_READ);
    jcc(t, XC_NE, slow);
    if (size > 1) {
        x_mov_rr(b, 4, RCX, addr);
        x_alu_ri(b, XA_AND, 4, RCX, (1u << JIT_MAP_SHIFT) - 1);
        x_alu_ri(b, XA_CMP, 4, RCX, (1u << JIT_MAP_SHIFT) - size);
        jcc(t, XC_A, slow);
    }
    bind(t, cont);
    defer(t, slow, cont, bail, addr, size, write);
}

static void mem_check(tr *t, int addr, unsigned size, bool write, int bail)
{
    mem_check_x(t, addr, size, write, bail, true);
}

static void ea_index(tr *t, const jit_ea *e, int tmp, int *index)
{
    int32_t xd = t->delta[e->index];
    if (e->index_long && xd == 0) {
        *index = H(t, e->index);
    } else {
        x_lea(&t->b, tmp, xm(H(t, e->index), xd));
        if (!e->index_long)
            x_movsx_rr(&t->b, 2, tmp, tmp);
        *index = tmp;
    }
}

/* The address of a memory operand, not masked, into `dst` (RAX or RDX).
 * (An)+ and -(An) are counted in t->delta, applied after the reads. A
 * memory indirect operand reads its pointer here (checked: may bail). */
static void ea_address(tr *t, const jit_ea *e, int dst, int bail)
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
    if (e->index >= 0 && e->ind != 2)
        ea_index(t, e, dst, &index);
    if (base < 0 && index < 0)
        x_mov_ri(b, dst, (uint32_t)disp);
    else
        x_lea(b, dst, xmi(base, index, e->scale, disp));
    if (e->base >= 0 && e->step > 0)
        t->delta[e->base] += e->step;
    if (e->ind) {
        mem_check(t, dst, 4, false, bail);
        load_be(t, 4, dst, dst, 0);
        index = -1;
        if (e->index >= 0 && e->ind == 2)
            ea_index(t, e, RCX, &index);
        if (index >= 0 || e->od != 0)
            x_lea(b, dst, xmi(dst, index, e->scale, (int32_t)e->od));
    }
}

/* The masked address of a memory operand of `size` bytes into `dst`,
 * checked. */
static void mem_address(tr *t, const jit_ea *e, int dst, unsigned size,
                        bool write, int bail)
{
    xbuf *b = &t->b;
    if (jea_mem_const(e)) {
        /* Checked when decoded; a write may still hit translated code. */
        uint32_t a = e->disp & t->c->addr_mask;
        x_mov_ri(b, dst, a);
        t->naccess[t->cur]++;
        if (e->host) {
            /* (what follows does not run) */
            jmp(t, host_label(t));
        } else if (write) {
            int slow = new_label(t), cont = new_label(t);
            if ((a & ((1u << JIT_MAP_SHIFT) - 1)) + size
                > (1u << JIT_MAP_SHIFT)) {
                jmp(t, slow);
            } else {
                x_load64(b, RCX, xm(RCPU, OFF_JIT));
                x_test_mi8(b, xm(RCX, JOFF_MAP + (int32_t)(a >> JIT_MAP_SHIFT)),
                           JMAP_CODE);
                jcc(t, XC_NE, slow);
            }
            bind(t, cont);
            defer(t, slow, cont, bail, dst, size, true);
        }
        return;
    }
    ea_address(t, e, dst, bail);
    mem_check(t, dst, size, write, bail);
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
        load_be(t, size, RCX, addr, 0);
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

static int alu_mode(unsigned alu)
{
    if (alu == JA_AND || alu == JA_OR || alu == JA_EOR)
        return PM_LOGIC;
    return alu == JA_CMP ? PM_ARITH : PM_ARITHX;
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
                flags_pending(t, fl, PM_LOGIC);
            }
        }
        return;
    }
    if (v.reg >= 0 && fl)
        x_test_rr(b, (int)size, v.reg, v.reg);
    store_be(t, size, RDX, 0, v);
    if (v.reg < 0)
        flags_const(t, fl, nz_of(v.imm, size));
    else
        flags_pending(t, fl, PM_LOGIC);
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

/* ADDA/SUBA that become a LEA. */
static bool adda_lea(const jit_insn *in)
{
    return in->kind == JK_ADDA && in->alu != JA_CMP
        && (in->src.kind == JEA_IMM
            || ((in->src.kind == JEA_DREG || in->src.kind == JEA_AREG)
                && in->alu == JA_ADD));
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
    if (adda_lea(in)) {
        /* no flags: a LEA keeps pending ones in EFLAGS */
        if (v.reg < 0)
            x_lea(b, d, xm(d, (int32_t)(in->alu == JA_SUB ? 0u - v.imm
                                                          : v.imm)));
        else
            x_lea(b, d, xmi(d, v.reg, 0, 0));
        return;
    }
    op_alu_ri_or_rr(t, x86_alu(in->alu), 4, d, v);
    if (in->alu == JA_CMP)
        flags_pending(t, fl, PM_ARITH);
}

static void emit_alu(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    int op = x86_alu(in->alu);
    if (in->dst.kind == JEA_DREG) {
        if (in->src.kind == JEA_MEM)
            mem_address(t, &in->src, RAX, size, false, bail);
        val v = src_value(t, &in->src, size, RAX);
        apply_deltas(t);
        op_alu_ri_or_rr(t, op, size, H(t, in->dst.reg), v);
        flags_pending(t, fl, alu_mode(in->alu));
        return;
    }
    /* to memory: read, change, write */
    mem_address(t, &in->dst, RDX, size, in->alu != JA_CMP, bail);
    load_be(t, size, RCX, RDX, 0);
    val v = src_value(t, &in->src, size, -1);
    apply_deltas(t);
    op_alu_ri_or_rr(t, op, size, RCX, v);
    if (in->alu != JA_CMP)
        store_be(t, size, RDX, 0, vreg(RCX));
    flags_pending(t, fl, alu_mode(in->alu));
}

static void emit_cmpm(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    xbuf *b = &t->b;
    mem_address(t, &in->src, RAX, size, false, bail);
    mem_address(t, &in->dst, RDX, size, false, bail);
    load_be(t, size, RCX, RAX, 0);
    x_mov_rr(b, 4, RAX, RCX);
    load_be(t, size, RCX, RDX, 0);
    apply_deltas(t);
    x_alu_rr(b, XA_CMP, (int)size, RCX, RAX);
    flags_pending(t, fl, PM_ARITH);
}

static void emit_tst(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    apply_deltas(t);
    if (v.reg < 0) {
        flags_const(t, fl, nz_of(v.imm, size));
    } else if (fl) {
        x_test_rr(&t->b, (int)size, v.reg, v.reg);
        flags_pending(t, fl, PM_LOGIC);
    }
}

static void emit_clr(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    unsigned size = in->size;
    if (in->dst.kind == JEA_MEM) {
        mem_address(t, &in->dst, RDX, size, true, bail);
        apply_deltas(t);
        val zero = { -1, 0 };
        store_be(t, size, RDX, 0, zero);
    } else {
        x_mov_ri_sz(&t->b, (int)size, H(t, in->dst.reg), 0);
    }
    flags_const(t, fl, JF_Z);
}

/* The operand of a read-modify-write instruction: its x86 register, or
 * RCX loaded from the checked address in RDX. */
static int rmw_begin(tr *t, const jit_insn *in, int bail)
{
    if (in->dst.kind != JEA_MEM)
        return H(t, in->dst.reg);
    mem_address(t, &in->dst, RDX, in->size, true, bail);
    load_be(t, in->size, RCX, RDX, 0);
    return RCX;
}

static void rmw_end(tr *t, const jit_insn *in)
{
    if (in->dst.kind == JEA_MEM)
        store_be(t, in->size, RDX, 0, vreg(RCX));
}

static void emit_unary(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    int size = in->size;
    int d = rmw_begin(t, in, bail);
    apply_deltas(t);
    switch (in->sub) {
    case JU_NEG:
        x_unary(b, XU_NEG, size, d);
        rmw_end(t, in);
        flags_pending(t, fl, PM_ARITHX);
        break;
    case JU_NOT:
        x_unary(b, XU_NOT, size, d);
        if (fl)
            x_test_rr(b, size, d, d);
        rmw_end(t, in);
        flags_pending(t, fl, PM_LOGIC);
        break;
    default:    /* NEGX: 0 - d - X */
        x_btx_ri(b, XB_BT, RSR, 4);
        x_mov_ri(b, RAX, 0);
        x_alu_rr(b, XA_SBB, size, RAX, d);
        x_mov_rr(b, size, d, RAX);
        rmw_end(t, in);
        flags_sticky(t, fl);
        break;
    }
}

static void emit_addx(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    int size = in->size, op = in->sub ? XA_SBB : XA_ADC;
    if (in->dst.kind == JEA_DREG) {
        x_btx_ri(b, XB_BT, RSR, 4);
        x_alu_rr(b, op, size, H(t, in->dst.reg), H(t, in->src.reg));
        flags_sticky(t, fl);
        return;
    }
    mem_address(t, &in->src, RAX, (unsigned)size, false, bail);
    mem_address(t, &in->dst, RDX, (unsigned)size, true, bail);
    load_be(t, (unsigned)size, RCX, RAX, 0);
    x_mov_rr(b, 4, RAX, RCX);
    load_be(t, (unsigned)size, RCX, RDX, 0);
    apply_deltas(t);
    x_btx_ri(b, XB_BT, RSR, 4);
    x_alu_rr(b, op, size, RCX, RAX);
    store_be(t, (unsigned)size, RDX, 0, vreg(RCX));
    flags_sticky(t, fl);
}

/* Shifts and rotates by 1-8 on Dn, or by 1 on a word in memory. */
static void emit_shift(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    int size = in->size, k = in->count;
    bool mem = in->dst.kind == JEA_MEM;
    int bit = mem ? RAX : RDX;      /* where a C bit is kept for later */
    int d = rmw_begin(t, in, bail);
    apply_deltas(t);
    int csrc = FB_CF, vsrc = FB_ZERO;
    bool x_eq_c = true, pend = false;
    switch (in->sub) {
    case JS_AS:
        if (k == 1) {
            /* x86's OF of a shift by one is the 68k's V for ASL and ASR */
            x_shift_r(b, in->left ? XS_SHL : XS_SAR, size, d, 1);
            pend = true;
        } else if (in->left) {
            /* V: the top k + 1 bits are not all equal (Dn only) */
            x_mov_rr(b, 4, RAX, d);
            if (size < 4)
                x_shift_ri(b, 4, RAX, 32 - 8 * size);
            x_shift_r(b, XS_SAR, 4, RAX, 31 - k);
            x_alu_ri(b, XA_ADD, 4, RAX, 1);
            x_alu_ri(b, XA_CMP, 4, RAX, 1);
            x_setcc(b, XC_A, RDX);
            x_shift_r(b, XS_SHL, size, d, k);
            vsrc = FB_DL;
        } else {
            x_shift_r(b, XS_SAR, size, d, k);
        }
        break;
    case JS_LS:
        x_shift_r(b, in->left ? XS_SHL : XS_SHR, size, d, k);
        break;
    case JS_RO:
        x_shift_r(b, in->left ? XS_ROL : XS_ROR, size, d, k);
        x_setcc(b, XC_B, bit);
        x_test_rr(b, size, d, d);
        csrc = FB_DL;
        x_eq_c = false;
        break;
    default:    /* ROX */
        x_btx_ri(b, XB_BT, RSR, 4);
        x_shift_r(b, in->left ? XS_RCL : XS_RCR, size, d, k);
        x_setcc(b, XC_B, bit);
        x_test_rr(b, size, d, d);
        csrc = FB_DL;
        break;
    }
    rmw_end(t, in);
    if (pend) {
        flags_pending(t, fl, PM_ARITHX);
        return;
    }
    if (csrc == FB_DL && bit != RDX)
        x_mov_rr(b, 4, RDX, RAX);   /* no flags changed */
    flags_build(t, fl, csrc, vsrc, x_eq_c);
}

/* BTST, BCHG, BCLR, BSET. */
static void emit_bit(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    static const int ops[4] = { XB_BT, XB_BTC, XB_BTR, XB_BTS };
    xbuf *b = &t->b;
    int op = ops[in->sub & 3];
    if (in->dst.kind == JEA_DREG) {
        int d = H(t, in->dst.reg);
        if (in->src.kind == JEA_IMM)
            x_btx_ri(b, op, d, (int)(in->src.disp & 31));
        else
            x_btx_rr(b, op, d, H(t, in->src.reg));
    } else {
        if (in->dst.kind == JEA_IMM) {
            x_mov_ri(b, RCX, in->dst.disp & 0xFF);
        } else {
            mem_address(t, &in->dst, RDX, 1, in->sub != JB_TST, bail);
            load_be(t, 1, RCX, RDX, 0);
            apply_deltas(t);
        }
        if (in->src.kind == JEA_IMM) {
            x_btx_ri(b, op, RCX, (int)(in->src.disp & 7));
        } else {
            x_mov_rr(b, 4, RAX, H(t, in->src.reg));
            x_alu_ri(b, XA_AND, 4, RAX, 7);
            x_btx_rr(b, op, RCX, RAX);
        }
        if (in->sub != JB_TST)
            store_be(t, 1, RDX, 0, vreg(RCX));
    }
    if (fl & JF_Z) {
        x_setcc(b, XC_AE, RAX);         /* Z = !CF */
        x_movzx_rr(b, 1, RAX, RAX);
        x_shift_ri(b, 4, RAX, 2);
        x_alu_ri(b, XA_AND, 4, RSR, ~JF_Z);
        x_alu_rr(b, XA_OR, 4, RSR, RAX);
    }
}

/* MULU.W, MULS.W */
static void emit_mul(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    bool sign = in->sub;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, 2, false, bail);
    val v = src_value(t, &in->src, 2, RAX);
    apply_deltas(t);
    int d = H(t, in->dst.reg);
    if (v.reg < 0)
        x_mov_ri(b, RCX, sign ? sext(v.imm, SZ_W) : v.imm & 0xFFFF);
    else if (sign)
        x_movsx_rr(b, 2, RCX, v.reg);
    else
        x_movzx_rr(b, 2, RCX, v.reg);
    if (sign)
        x_movsx_rr(b, 2, RAX, d);
    else
        x_movzx_rr(b, 2, RAX, d);
    x_imul_rr(b, 4, RAX, RCX);
    x_mov_rr(b, 4, d, RAX);
    if (fl) {
        x_test_rr(b, 4, RAX, RAX);
        flags_pending(t, fl, PM_LOGIC);
    }
}

/* The source of a divide, read in the first phase: a zero divisor leaves
 * for the interpreter (the exception). */
static val div_source(tr *t, const jit_insn *in, unsigned size, int bail)
{
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    if (v.reg >= 0) {
        x_test_rr(&t->b, (int)size, v.reg, v.reg);
        jcc(t, XC_E, bail);
    }
    apply_deltas(t);
    return v;
}

/* Overflow: N and V set, Z and C clear; the registers do not change. */
static void div_end(tr *t, unsigned fl, int ovf, int done)
{
    jmp(t, done);
    bind(t, ovf);
    flags_const(t, fl, JF_N | JF_V);
    bind(t, done);
}

/* DIVU.W, DIVS.W */
static void emit_div(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    bool sign = in->sub;
    val v = div_source(t, in, 2, bail);
    int d = H(t, in->dst.reg);
    int ovf = new_label(t), done = new_label(t);
    if (!sign) {
        if (v.reg < 0)
            x_mov_ri(b, RCX, v.imm & 0xFFFF);
        else
            x_movzx_rr(b, 2, RCX, v.reg);
        x_mov_rr(b, 4, RAX, d);
        x_mov_ri(b, RDX, 0);
        x_unary(b, XU_DIV, 4, RCX);
        x_alu_ri(b, XA_CMP, 4, RAX, 0xFFFF);
        jcc(t, XC_A, ovf);
    } else {
        if (v.reg < 0) {
            x_mov_ri(b, RCX, sext(v.imm, SZ_W));
            x_movsxd(b, RCX, RCX);
        } else {
            x_movsx64_rr(b, 2, RCX, v.reg);
        }
        x_movsxd(b, RAX, d);
        x_cqo(b);
        x_unary(b, XU_IDIV, 8, RCX);
        x_movsx64_rr(b, 2, RCX, RAX);
        x_alu_rr(b, XA_CMP, 8, RCX, RAX);
        jcc(t, XC_NE, ovf);
    }
    x_shift_ri(b, 4, RDX, 16);
    x_movzx_rr(b, 2, RAX, RAX);
    x_alu_rr(b, XA_OR, 4, RDX, RAX);
    x_mov_rr(b, 4, d, RDX);
    if (fl) {
        x_test_rr(b, 2, RAX, RAX);
        flags_from_eflags(t, fl, PM_LOGIC);
    }
    div_end(t, fl, ovf, done);
}

/* MULU.L, MULS.L: the product in 64 bits; N and Z from all of it. */
static void emit_mull(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    unsigned ext = in->ext;
    int dl = H(t, (ext >> 12) & 7), dh = H(t, ext & 7);
    bool sign = (ext & 0x0800) != 0, wide = (ext & 0x0400) != 0;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, 4, false, bail);
    val v = src_value(t, &in->src, 4, RAX);
    apply_deltas(t);
    if (v.reg < 0)
        x_mov_ri(b, RCX, v.imm);
    else
        x_mov_rr(b, 4, RCX, v.reg);
    if (sign) {
        x_movsxd(b, RCX, RCX);
        x_movsxd(b, RAX, dl);
    } else {
        x_mov_rr(b, 4, RAX, dl);
    }
    x_imul_rr(b, 8, RAX, RCX);
    if (wide) {
        x_mov_rr(b, 8, RDX, RAX);
        x_shift_r(b, XS_SHR, 8, RDX, 32);
        x_mov_rr(b, 4, dh, RDX);
        x_mov_rr(b, 4, dl, RAX);
        if (fl) {
            x_test_rr(b, 8, RAX, RAX);
            flags_pending(t, fl, PM_LOGIC);
        }
        return;
    }
    x_mov_rr(b, 4, dl, RAX);
    if (!fl)
        return;
    /* V: the high half is not just the extension of the low one */
    if (sign) {
        x_movsxd(b, RDX, RAX);
        x_alu_rr(b, XA_CMP, 8, RDX, RAX);
        x_setcc(b, XC_NE, RDX);
    } else {
        x_mov_rr(b, 8, RDX, RAX);
        x_shift_r(b, XS_SHR, 8, RDX, 32);
        x_setcc(b, XC_NE, RDX);
    }
    x_test_rr(b, 8, RAX, RAX);
    flags_build(t, fl, FB_ZERO, FB_DL, false);
}

/* DIVU.L, DIVS.L, DIVUL.L, DIVSL.L (see the interpreter's op_div_l). */
static void emit_divl(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    unsigned ext = in->ext;
    int dq = H(t, (ext >> 12) & 7), dr = H(t, ext & 7);
    bool sign = (ext & 0x0800) != 0, wide = (ext & 0x0400) != 0;
    val v = div_source(t, in, 4, bail);
    int ovf = new_label(t), done = new_label(t);
    if (v.reg < 0)
        x_mov_ri(b, RCX, v.imm);
    else if (v.reg != RCX)
        x_mov_rr(b, 4, RCX, v.reg);
    if (sign)
        x_movsxd(b, RCX, RCX);
    if (wide) {
        x_mov_rr(b, 4, RAX, dq);
        x_mov_rr(b, 4, RDX, dr);
        x_shift_r(b, XS_SHL, 8, RDX, 32);
        x_alu_rr(b, XA_OR, 8, RAX, RDX);
    } else if (sign) {
        x_movsxd(b, RAX, dq);
    } else {
        x_mov_rr(b, 4, RAX, dq);
    }
    if (!sign) {
        x_mov_ri(b, RDX, 0);
        x_unary(b, XU_DIV, 8, RCX);
        x_mov_rr(b, 8, RCX, RAX);
        x_shift_r(b, XS_SHR, 8, RCX, 32);
        jcc(t, XC_NE, ovf);
    } else {
        /* x86 traps on the most negative dividend divided by -1 */
        int m1 = new_label(t), chk = new_label(t);
        x_alu_ri(b, XA_CMP, 8, RCX, 0xFFFFFFFFu);
        jcc(t, XC_E, m1);
        x_cqo(b);
        x_unary(b, XU_IDIV, 8, RCX);
        jmp(t, chk);
        bind(t, m1);
        x_unary(b, XU_NEG, 8, RAX);
        x_mov_ri(b, RDX, 0);
        bind(t, chk);
        x_movsxd(b, RCX, RAX);
        x_alu_rr(b, XA_CMP, 8, RCX, RAX);
        jcc(t, XC_NE, ovf);
    }
    x_mov_rr(b, 4, dr, RDX);
    x_mov_rr(b, 4, dq, RAX);
    if (fl) {
        x_test_rr(b, 4, RAX, RAX);
        flags_from_eflags(t, fl, PM_LOGIC);
    }
    div_end(t, fl, ovf, done);
}

static void emit_ext(tr *t, const jit_insn *in, unsigned fl)
{
    xbuf *b = &t->b;
    int d = H(t, in->dst.reg);
    switch (in->sub) {
    case JE_W:
        x_movsx_rr(b, 1, RCX, d);
        x_mov_rr(b, 2, d, RCX);
        if (fl)
            x_test_rr(b, 2, RCX, RCX);
        break;
    case JE_L:
        x_movsx_rr(b, 2, d, d);
        if (fl)
            x_test_rr(b, 4, d, d);
        break;
    default:
        x_movsx_rr(b, 1, d, d);
        if (fl)
            x_test_rr(b, 4, d, d);
        break;
    }
    flags_pending(t, fl, PM_LOGIC);
}

static void emit_scc(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    unsigned cc = in->cc;
    if (in->dst.kind == JEA_DREG) {
        int d = H(t, in->dst.reg);
        if (cc <= 1) {
            x_mov_ri_sz(b, 1, d, cc == 0 ? 0xFF : 0);
        } else {
            x_setcc(b, cond(t, cc, in->flags_use), RDX);
            flags_flush(t);
            x_unary(b, XU_NEG, 1, RDX);
            x_mov_rr(b, 1, d, RDX);
        }
        return;
    }
    mem_address(t, &in->dst, RDX, 1, true, bail);
    apply_deltas(t);
    if (cc <= 1) {
        val c = { -1, cc == 0 ? 0xFF : 0 };
        store_be(t, 1, RDX, 0, c);
        return;
    }
    x_setcc(b, cond(t, cc, in->flags_use), RAX);
    x_unary(b, XU_NEG, 1, RAX);
    store_be(t, 1, RDX, 0, vreg(RAX));
}

static int popcount16(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static void emit_movem(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    unsigned size = in->size, total = (unsigned)popcount16(in->mask) * size;
    bool to_regs = in->sub;
    const jit_ea *e = to_regs ? &in->src : &in->dst;
    unsigned mode = (in->op >> 3) & 7;
    int an = mode >= 2 && mode <= 6 ? e->base : -1;
    if (mode == 4) {
        x_lea(b, RAX, xm(H(t, an), -(int32_t)total));
        mem_check(t, RAX, total, true, bail);
    } else {
        mem_address(t, e, RAX, total, !to_regs, bail);
    }
    if (mode == 3)
        x_lea(b, RDX, xm(H(t, an), (int32_t)total));
    unsigned k = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (!(in->mask & (1u << i)))
            continue;
        if (mode == 4) {
            /* -(An): bit 0 is A7, stored at the highest address */
            int r = 15 - (int)i;
            k++;
            int v = reg_get(t, r, RCX);
            store_be(t, size, RAX, (int32_t)(total - k * size), vreg(v));
            continue;
        }
        int r = (int)i;
        int32_t off = (int32_t)(k * size);
        k++;
        if (!to_regs) {
            int v = reg_get(t, r, RCX);
            store_be(t, size, RAX, off, vreg(v));
            continue;
        }
        int d = H(t, r) >= 0 ? H(t, r) : RCX;
        load_be(t, size, d, RAX, off);
        if (size == 2)
            x_movsx_rr(b, 2, d, d);
        if (d == RCX)
            x_store(b, 4, xm(RCPU, OFF_REG(r)), RCX);
    }
    if (mode == 4)
        x_lea(b, H(t, an), xm(H(t, an), -(int32_t)total));
    else if (mode == 3)
        x_mov_rr(b, 4, H(t, an), RDX);
}

/* LINK An,#d: push An, An = SP, SP += d. LINK A7 pushes the SP from
 * before the push. */
static void emit_link(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    int sp = H(t, JR_A(7)), an = H(t, in->dst.reg);
    x_lea(b, RAX, xm(sp, -4));
    mem_check(t, RAX, 4, true, bail);
    store_be(t, 4, RAX, 0, vreg(an));
    x_lea(b, sp, xm(sp, -4));
    if (an != sp)
        x_mov_rr(b, 4, an, sp);
    x_lea(b, sp, xm(sp, (int32_t)in->src.disp));
}

/* UNLK An: SP = An + 4, An = (An). */
static void emit_unlk(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    int sp = H(t, JR_A(7)), an = H(t, in->dst.reg);
    x_mov_rr(b, 4, RAX, an);
    mem_check(t, RAX, 4, false, bail);
    load_be(t, 4, RCX, RAX, 0);
    x_lea(b, sp, xm(an, 4));
    x_mov_rr(b, 4, an, RCX);
}

static void emit_pea(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    int sp = H(t, JR_A(7));
    ea_address(t, &in->src, RDX, bail);
    x_lea(b, RAX, xm(sp, -4));
    mem_check(t, RAX, 4, true, bail);
    store_be(t, 4, RAX, 0, vreg(RDX));
    x_lea(b, sp, xm(sp, -4));
}

/* An FPU instruction runs from the block only if it cannot take an
 * exception: none pending from the one before, and none enabled in the
 * FPCR (so this one leaves none pending either, and BSUN is off). Else
 * it bails. Uses RAX and RCX. */
static void fpu_check(tr *t, int bail)
{
    xbuf *b = &t->b;
    x_load(b, 4, RAX, xm(RCPU, OFF_FPCR));
    x_alu_ri(b, XA_AND, 4, RAX, 0xFF00);
    x_load(b, 4, RCX, xm(RCPU, OFF_FPEND));
    x_alu_rr(b, XA_OR, 4, RAX, RCX);
    jcc(t, XC_NE, bail);
}

/* Calls the interpreter's handler: the registers it may read are stored
 * first, those it may change loaded after. A bit field in memory first
 * checks the 5 bytes from the byte its offset points into. */
static void emit_call(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    if (in->sub == 2) {
        /* ABCD, SBCD -(Ay),-(Ax): the source byte, then the destination
         * (one more step down when Ax is Ay) */
        int y = in->src.reg, x = in->dst.reg;
        int32_t sy = y == JR_A(7) ? 2 : 1, sx = x == JR_A(7) ? 2 : 1;
        x_lea(b, RAX, xm(H(t, y), -sy));
        mem_check_x(t, RAX, 1, false, bail, false);
        x_lea(b, RDX, xm(H(t, x), -sx - (x == y ? sy : 0)));
        mem_check_x(t, RDX, 1, true, bail, false);
    } else if (in->sub == 3) {
        /* NBCD <ea>: the handler moves An itself */
        if (jea_mem_const(&in->dst)) {
            mem_address(t, &in->dst, RAX, 1, true, bail);
        } else {
            ea_address(t, &in->dst, RAX, bail);
            mem_check_x(t, RAX, 1, true, bail, false);
        }
        memset(t->delta, 0, sizeof t->delta);
    } else if (in->sub == 1) {
        if (jea_mem_const(&in->dst) && !(in->ext & 0x0800)) {
            jit_ea e = in->dst;
            e.disp += ((unsigned)in->ext >> 6 & 31) >> 3;
            mem_address(t, &e, RAX, 5, in->write, bail);
        } else {
            ea_address(t, &in->dst, RAX, bail);
            if (in->ext & 0x0800) {
                x_mov_rr(b, 4, RCX, H(t, (in->ext >> 6) & 7));
                x_shift_r(b, XS_SAR, 4, RCX, 3);
                x_alu_rr(b, XA_ADD, 4, RAX, RCX);
            } else if (((unsigned)in->ext >> 6 & 31) >> 3) {
                x_alu_ri(b, XA_ADD, 4, RAX, ((unsigned)in->ext >> 6 & 31) >> 3);
            }
            mem_check(t, RAX, 5, in->write, bail);
        }
    }
    static const int saved[] = { RSI, RDI, R8, R9, R10, R11 };
    store_regs(t);
    x_store(b, 2, xm(RCPU, OFF_SR), RSR);
    x_store_imm(b, 4, xm(RCPU, OFF_PC), in->pc + 2);
    for (int i = 0; i < 6; i++)
        x_push(b, saved[i]);
    relevo_handler h = t->c->table[in->op];
    uint64_t addr;
    memcpy(&addr, &h, sizeof addr);
#if defined(_WIN32)
    x_rsp_adjust(b, -32);
    x_mov_rr(b, 8, RCX, RCPU);
    x_mov_ri(b, RDX, in->op);
#else
    x_mov_rr(b, 8, RDI, RCPU);
    x_mov_ri(b, RSI, in->op);
#endif
    x_mov_ri64(b, RAX, addr);
    x_call_r(b, RAX);
#if defined(_WIN32)
    x_rsp_adjust(b, 32);
#endif
    for (int i = 6; i-- > 0;)
        x_pop(b, saved[i]);
    for (int r = 0; r < 16; r++)
        if ((in->wregs & (1u << r)) && H(t, r) >= 0)
            x_load(b, 4, H(t, r), xm(RCPU, OFF_REG(r)));
    x_movzx_load(b, 2, RSR, xm(RCPU, OFF_SR));
}

/* TAS: N and Z of the byte, then bit 7 set. */
static void emit_tas(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    int d = rmw_begin(t, in, bail);
    apply_deltas(t);
    x_mov_rr(b, 4, RAX, d);
    x_alu_ri(b, XA_OR, 1, d, 0x80);
    rmw_end(t, in);
    if (fl) {
        x_test_rr(b, 1, RAX, RAX);
        flags_pending(t, fl, PM_LOGIC);
    }
}

/* CHK: Dn < 0 or Dn > the bound takes the exception (the interpreter's,
 * after a bail). Otherwise the 68000 and 68010 set Z from Dn and clear N,
 * V and C; later models change nothing. */
static void emit_chk(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    unsigned size = in->size;
    if (in->src.kind == JEA_MEM)
        mem_address(t, &in->src, RAX, size, false, bail);
    val v = src_value(t, &in->src, size, RAX);
    int dn = H(t, in->dst.reg);
    if (size == 2) {
        x_movsx_rr(b, 2, RAX, dn);
        if (v.reg >= 0) {
            x_movsx_rr(b, 2, RCX, v.reg);
            v.reg = RCX;
        } else {
            v.imm = sext(v.imm, SZ_W);
        }
    } else {
        x_mov_rr(b, 4, RAX, dn);
    }
    x_test_rr(b, 4, RAX, RAX);
    jcc(t, XC_S, bail);
    op_alu_ri_or_rr(t, XA_CMP, 4, RAX, v);
    jcc(t, XC_G, bail);
    apply_deltas(t);
    if (fl) {
        x_test_rr(b, 4, RAX, RAX);
        flags_pending(t, fl, PM_LOGIC);
    }
}

/* CAS Dc,Du,<ea>: flags of <ea> - Dc; equal, Du goes to <ea>; else <ea>
 * goes to Dc. (Both paths keep EFLAGS.) */
static void emit_cas(tr *t, const jit_insn *in, unsigned fl, int bail)
{
    xbuf *b = &t->b;
    int size = in->size;
    mem_address(t, &in->dst, RDX, in->size, true, bail);
    load_be(t, in->size, RCX, RDX, 0);
    apply_deltas(t);
    int dc = H(t, in->ext & 7), du = H(t, (in->ext >> 6) & 7);
    int ne = new_label(t), done = new_label(t);
    x_alu_rr(b, XA_CMP, size, RCX, dc);
    jcc(t, XC_NE, ne);
    store_be(t, in->size, RDX, 0, vreg(du));
    jmp(t, done);
    bind(t, ne);
    x_mov_rr(b, size, dc, RCX);
    bind(t, done);
    flags_pending(t, fl, PM_ARITH);
}

/* MOVEP: the bytes of Dn, high first, at every other address. */
static void emit_movep(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    unsigned n = in->size;
    int dn = H(t, in->src.reg);
    x_lea(b, RDX, xm(H(t, in->dst.base), (int32_t)in->dst.disp));
    mem_check_x(t, RDX, 2 * n - 1, in->sub, bail, false);
    for (unsigned i = 0; i < n; i++) {
        if (in->sub) {
            x_mov_rr(b, 4, RCX, dn);
            if (i + 1 < n)
                x_shift_ri(b, 5, RCX, (int)(8 * (n - 1 - i)));
            x_store(b, 1, at68k(RDX, (int32_t)(2 * i)), RCX);
        } else if (i == 0) {
            x_movzx_load(b, 1, RAX, at68k(RDX, 0));
        } else {
            x_movzx_load(b, 1, RCX, at68k(RDX, (int32_t)(2 * i)));
            x_shift_ri(b, 4, RAX, 8);
            x_alu_rr(b, XA_OR, 4, RAX, RCX);
        }
    }
    if (!in->sub)
        x_mov_rr(b, (int)n, dn, RAX);
}

/* A call of fpu_jit_op() or fpu_jit_branch(): the x86 registers that hold
 * 68k registers and that C may change are kept on the stack (an even
 * number: RSP stays 16-byte aligned). EAX is the fourth argument; the
 * result is in RAX. */
static void call_fpu(tr *t, const void *fnp, size_t fnsize, uint32_t a1,
                     uint32_t a2)
{
    xbuf *b = &t->b;
    static const int saved[] = { RSI, RDI, R8, R9, R10, R11 };
    uint64_t addr = 0;
    memcpy(&addr, fnp, fnsize);
    for (int i = 0; i < 6; i++)
        x_push(b, saved[i]);
#if defined(_WIN32)
    x_rsp_adjust(b, -32);
    x_mov_rr(b, 4, R9, RAX);
    x_mov_rr(b, 8, RCX, RCPU);
    x_mov_ri(b, RDX, a1);
    x_mov_ri(b, R8, a2);
#else
    x_mov_rr(b, 4, RCX, RAX);
    x_mov_rr(b, 8, RDI, RCPU);
    x_mov_ri(b, RSI, a1);
    x_mov_ri(b, RDX, a2);
#endif
    x_mov_ri64(b, RAX, addr);
    x_call_r(b, RAX);
#if defined(_WIN32)
    x_rsp_adjust(b, 32);
#endif
    for (int i = 6; i-- > 0;)
        x_pop(b, saved[i]);
}

/* An FPU instruction: its operand addressed and checked (An moved here),
 * or a Dn read or written here; the rest in fpu_jit_op(). */
static void emit_fpu(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    bool out = (in->ext >> 13) == 3;
    fpu_check(t, bail);
    if (in->dst.kind == JEA_MEM) {
        mem_address(t, &in->dst, RAX, in->size, in->write, bail);
        apply_deltas(t);
    } else if (in->dst.kind == JEA_DREG && !out) {
        x_mov_rr(b, 4, RAX, H(t, in->dst.reg));
    }
    uint32_t (*fn)(relevo_cpu *, uint32_t, uint32_t, uint32_t) = fpu_jit_op;
    call_fpu(t, &fn, sizeof fn, in->pc,
             in->ext | (uint32_t)(in->op & 0x3F) << 16);
    if (in->dst.kind == JEA_DREG && out)
        x_mov_rr(b, in->size, H(t, in->dst.reg), RAX);
}

/* FBcc: the condition from fpu_jit_branch(), then as Bcc. */
static void emit_fbcc(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    fpu_check(t, bail);
    bool (*fn)(relevo_cpu *, uint32_t, unsigned) = fpu_jit_branch;
    call_fpu(t, &fn, sizeof fn, in->pc, in->cc);
    int taken = new_label(t);
    x_test_rr(b, 1, RAX, RAX);
    jcc(t, XC_NE, taken);
    fstate f = fs_save(t);
    exit_chain(t, in->pc + in->len);
    bind(t, taken);
    fs_restore(t, f);
    exit_branch(t, in->target);
}

static void emit_bcc(tr *t, const jit_insn *in)
{
    uint32_t next = in->pc + in->len;
    if (in->cc == 0) {
        exit_branch(t, in->target);
        return;
    }
    int taken = new_label(t);
    int x = cond(t, in->cc, in->flags_use);
    flags_settle(t);
    jcc(t, x, taken);
    fstate f = fs_save(t);
    exit_chain(t, next);
    bind(t, taken);
    fs_restore(t, f);
    exit_branch(t, in->target);
}

static void emit_dbcc(tr *t, const jit_insn *in)
{
    uint32_t next = in->pc + in->len;
    if (in->cc == 0) {          /* DBT: never loops */
        exit_chain(t, next);
        return;
    }
    int out_true = new_label(t), m1 = new_label(t);
    int x = in->cc != 1 ? cond(t, in->cc, in->flags_use) : 0;
    flags_settle(t);
    fstate f = fs_save(t);
    if (in->cc != 1)
        jcc(t, x, out_true);
    flags_flush(t);
    /* Dn.W - 1: CF when it was 0, i.e. the counter is now -1 */
    x_alu_ri(&t->b, XA_SUB, 2, H(t, in->dst.reg), 1);
    fstate g = fs_save(t);
    jcc(t, XC_B, m1);
    exit_branch(t, in->target);
    bind(t, m1);
    fs_restore(t, g);
    exit_chain(t, next);
    if (in->cc != 1) {
        bind(t, out_true);
        fs_restore(t, f);
        exit_chain(t, next);
    }
}

/* BSR and JSR: push the return address (checked: a long at A7 - 4). */
static void push_return(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    int sp = H(t, JR_A(7));
    x_lea(b, RAX, xm(sp, -4));
    mem_check(t, RAX, 4, true, bail);
    val ret = { -1, in->pc + in->len };
    store_be(t, 4, RAX, 0, ret);
    x_lea(b, sp, xm(sp, -4));
}

static void emit_jump(tr *t, const jit_insn *in, int bail)
{
    xbuf *b = &t->b;
    switch (in->kind) {
    case JK_BSR:
        push_return(t, in, bail);
        exit_branch(t, in->target);
        return;
    case JK_JMP:
    case JK_JSR:
        if (jea_mem_const(&in->src)) {
            if (in->kind == JK_JSR)
                push_return(t, in, bail);
            exit_branch(t, in->src.disp);
            return;
        }
        ea_address(t, &in->src, RDX, bail);
        x_test_ri(b, 1, RDX, 1);    /* odd: address error */
        jcc(t, XC_NE, bail);
        if (in->kind == JK_JSR)
            push_return(t, in, bail);
        exit_indirect(t, RDX);
        return;
    default: {  /* RTS */
        int sp = H(t, JR_A(7));
        x_mov_rr(b, 4, RAX, sp);
        mem_check(t, RAX, 4, false, bail);
        load_be(t, 4, RDX, RAX, 0);
        x_test_ri(b, 1, RDX, 1);
        jcc(t, XC_NE, bail);
        x_lea(b, sp, xm(sp, 4));
        exit_indirect(t, RDX);
        return;
    }
    }
}

/* Whether an instruction can use pending flags as they are in EFLAGS
 * (it does not change EFLAGS before reading them). */
static bool reads_eflags(const jit_insn *in)
{
    return in->kind == JK_BCC || in->kind == JK_DBCC
        || (in->kind == JK_SCC && in->dst.kind == JEA_DREG);
}

/* Whether an instruction leaves EFLAGS alone (and never bails): pending
 * flags can wait across it. */
static bool keeps_eflags(const jit_insn *in)
{
    switch (in->kind) {
    case JK_NOP:
    case JK_EXG:
        return true;
    case JK_LEA:
        return !in->src.ind;
    case JK_MOVEA:
        return in->src.kind != JEA_MEM;
    case JK_ADDA:
        return adda_lea(in);
    default:
        return false;
    }
}

/* Whether an instruction reads its condition with cond(), which can take
 * the flags back from the record. */
static bool reads_cond(const jit_insn *in)
{
    return in->kind == JK_BCC || in->kind == JK_DBCC || in->kind == JK_SCC;
}

static void emit_insn(tr *t, unsigned i)
{
    const jit_insn *in = &t->in[i];
    unsigned fl = in->flags_def & t->live_out[i];
    xbuf *b = &t->b;
    int bail = -1;
    t->cur = i;
    if (in->may_bail) {
        bail = new_label(t);
        t->bail[i] = bail;
    }
    memset(t->delta, 0, sizeof t->delta);
    bool via_eflags = reads_eflags(in) && t->pend.mode != PM_NONE;
    if (!reads_eflags(in) && !keeps_eflags(in))
        flags_flush(t);
    unsigned need = in->flags_use;
    if (need != 0 && !via_eflags) {
        /* R14 must have what the instruction reads, but a condition can
         * also take all of it from the record. */
        unsigned miss = need & ~t->own;
        if (!(reads_cond(in) && miss == need && (miss & ~t->rec) == 0))
            flags_need(t, need);
    }
    t->own_at[i] = (uint8_t)t->own;
    switch (in->kind) {
    case JK_NOP:
        break;
    case JK_MOVEQ:
        x_mov_ri(b, H(t, in->dst.reg), in->src.disp);
        flags_const(t, fl, nz_of(in->src.disp, 4));
        break;
    case JK_MOVE:
        emit_move(t, in, fl, bail);
        break;
    case JK_MOVEA:
        emit_movea(t, in, bail);
        break;
    case JK_LEA:
        ea_address(t, &in->src, RAX, bail);
        x_mov_rr(b, 4, H(t, in->dst.reg), RAX);
        break;
    case JK_PEA:
        emit_pea(t, in, bail);
        break;
    case JK_ADDA:
        emit_adda(t, in, fl, bail);
        break;
    case JK_ALU:
        emit_alu(t, in, fl, bail);
        break;
    case JK_CMPM:
        emit_cmpm(t, in, fl, bail);
        break;
    case JK_TST:
        emit_tst(t, in, fl, bail);
        break;
    case JK_CLR:
        emit_clr(t, in, fl, bail);
        break;
    case JK_UNARY:
        emit_unary(t, in, fl, bail);
        break;
    case JK_ADDX:
        emit_addx(t, in, fl, bail);
        break;
    case JK_SHIFT:
        emit_shift(t, in, fl, bail);
        break;
    case JK_BIT:
        emit_bit(t, in, fl, bail);
        break;
    case JK_MUL:
        emit_mul(t, in, fl, bail);
        break;
    case JK_DIV:
        emit_div(t, in, fl, bail);
        break;
    case JK_MULL:
        emit_mull(t, in, fl, bail);
        break;
    case JK_DIVL:
        emit_divl(t, in, fl, bail);
        break;
    case JK_EXT:
        emit_ext(t, in, fl);
        break;
    case JK_SWAP:
        x_shift_r(b, XS_ROL, 4, H(t, in->dst.reg), 16);
        if (fl)
            x_test_rr(b, 4, H(t, in->dst.reg), H(t, in->dst.reg));
        flags_pending(t, fl, PM_LOGIC);
        break;
    case JK_EXG:
        if (in->src.reg != in->dst.reg)
            x_xchg_rr(b, H(t, in->src.reg), H(t, in->dst.reg));
        break;
    case JK_SCC:
        emit_scc(t, in, bail);
        break;
    case JK_MOVEM:
        emit_movem(t, in, bail);
        break;
    case JK_LINK:
        emit_link(t, in, bail);
        break;
    case JK_UNLK:
        emit_unlk(t, in, bail);
        break;
    case JK_TAS:
        emit_tas(t, in, fl, bail);
        break;
    case JK_CHK:
        emit_chk(t, in, fl, bail);
        break;
    case JK_CAS:
        emit_cas(t, in, fl, bail);
        break;
    case JK_MOVEP:
        emit_movep(t, in, bail);
        break;
    case JK_CALL:
        emit_call(t, in, bail);
        break;
    case JK_FBCC:
        emit_fbcc(t, in, bail);
        break;
    case JK_FPU:
        emit_fpu(t, in, bail);
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
    /* What the instruction wrote into R14 (what it left pending is in
     * t->pend; a handler call reloads all of SR). */
    if (in->kind == JK_CALL && in->flags_use == JF_ALL) {
        t->own = JF_ALL;
        t->rec = 0;
    } else if (fl != 0) {
        unsigned r14 = fl & ~(t->pend.mode != PM_NONE ? t->pend.mask : 0);
        t->own |= r14;
        t->rec &= ~r14;
    }
}

/* ---- Blocks ----------------------------------------------------------- */

/* The out-of-line half of the access checks: asks jit_access_check(). */
static void emit_deferred(tr *t)
{
    xbuf *b = &t->b;
    for (unsigned k = 0; k < t->ndefer; k++) {
        const deferred *d = &t->defer[k];
        bind(t, d->slow);
        x_mov_rr(b, 4, RCX, d->addr);
        x_push_imm(b, (int32_t)d->size_write);
        call_abs(t, t->j->check_off);
        x_lea64(b, RSP, xm(RSP, 8));
        x_alu_ri(b, XA_CMP, 4, RCX, 1);
        jcc(t, XC_E, d->cont);
        jcc(t, XC_B, d->bail);
        jmp(t, d->host);
    }
}

/* An access to a host page (or a ROM write) in instruction i: R14 and the
 * record made to agree, the registers and SR stored, the instructions
 * before counted; then the interpreter runs instruction i (jit_host_step)
 * and the block goes on to the next one through a chained jump, or leaves
 * for the dispatcher. Either way the state is in the CPU structure, so
 * nothing is stored again. */
static void emit_host_path(tr *t, unsigned i)
{
    xbuf *b = &t->b;
    const jit_insn *in = &t->in[i];
    uint32_t next = in->pc + in->len;
    bind(t, t->hostp[i]);
    t->pend.mode = PM_NONE;
    t->pend.mask = 0;
    t->own = t->own_at[i];
    flags_exit(t);
    store_regs(t);
    x_store(b, 2, xm(RCPU, OFF_SR), RSR);
    x_store_imm(b, 4, xm(RCPU, OFF_PC), in->pc);
    count(t, i);
    int (*fn)(relevo_cpu *, uint32_t, int) = jit_host_step;
    uint64_t addr;
    memcpy(&addr, &fn, sizeof addr);
    /* What can fault in the interpreter besides the access checked: other
     * memory operands, a jump to an odd address, and the frame of an
     * exception (division by zero, CHK) pushed to a missing page. */
    uint32_t may_fault = t->naccess[i] > 1 || in->ends_block
        || in->kind == JK_DIV || in->kind == JK_DIVL || in->kind == JK_CHK;
#if defined(_WIN32)
    x_rsp_adjust(b, -32);
    x_mov_rr(b, 8, RCX, RCPU);
    x_mov_ri(b, RDX, next);
    x_mov_ri(b, R8, may_fault);
#else
    x_mov_rr(b, 8, RDI, RCPU);
    x_mov_ri(b, RSI, next);
    x_mov_ri(b, RDX, may_fault);
#endif
    x_mov_ri64(b, RAX, addr);
    x_call_r(b, RAX);
#if defined(_WIN32)
    x_rsp_adjust(b, 32);
#endif
    x_movzx_load(b, 2, RSR, xm(RCPU, OFF_SR));
    int leave = new_label(t);
    x_test_rr(b, 4, RAX, RAX);
    jcc(t, XC_NE, leave);
    size_t at = x_jmp32(b);         /* chained to the block at `next` */
    x_mov_ri(b, RAX, next);
    x_mov_ri(b, RCX, JIT_EXIT_CHAIN);
    x_mov_ri(b, RDX, (uint32_t)(t->base + at));
    jmp_abs(t, t->j->exit_off);
    bind(t, leave);
    x_load(b, 4, RAX, xm(RCPU, OFF_PC));
    x_mov_ri(b, RCX, JIT_EXIT_NORMAL);
    jmp_abs(t, t->j->exit_off);
}

const uint8_t *jit_backend_translate(relevo_cpu *c, jit_state *j,
                                     unsigned n)
{
    tr *t = calloc(1, sizeof *t);
    if (t == NULL)
        return NULL;
    t->c = c;
    t->j = j;
    t->in = j->insns;
    t->n = n;
    t->movbe = j->movbe;
    t->lazy = j->lazy;
    t->own = t->lazy ? 0 : JF_ALL;
    t->base = j->used;
    t->b.p = j->mem.rw + j->used;
    t->b.cap = j->mem.size - j->used;

    /* Registers, by order of first use. */
    for (int r = 0; r < 16; r++)
        t->host[r] = -1;
    unsigned next = 0;
    for (unsigned i = 0; i < n; i++) {
        for (int r = 0; r < 16; r++) {
            if ((t->in[i].regs & (1u << r)) && t->host[r] < 0) {
                if (next >= JIT_HOST_REGS) {
                    free(t);
                    return NULL;    /* the block former keeps to 9 */
                }
                t->host[r] = host_pool[next++];
            }
        }
        t->used |= t->in[i].regs;
    }
    for (unsigned i = 0; i < n; i++)
        t->written |= t->in[i].wregs & t->used;

    /* Flags: which ones are read after each instruction (backwards). */
    unsigned live = JF_ALL;
    for (unsigned i = n; i-- > 0;) {
        const jit_insn *in = &t->in[i];
        t->live_out[i] = (uint8_t)live;
        live = (live & ~(unsigned)in->flags_def) | in->flags_use;
        if (in->may_bail)
            live = JF_ALL;
    }
    t->cyc_before[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        t->cyc_before[i + 1] = t->cyc_before[i] + t->in[i].cycles;
        t->bail[i] = -1;
        t->hostp[i] = -1;
    }

    t->tail = new_label(t);
    t->loop = new_label(t);
    t->entry_out = new_label(t);
    xbuf *b = &t->b;

    /* Entry: enough budget and no attention needed, then the registers
     * (SR is in R14 already). */
    x_alu_mi(b, XA_CMP, 8, xm(RCPU, OFF_BUDGET), (int32_t)n);
    jcc(t, XC_B, t->entry_out);
    x_alu_mi(b, XA_CMP, 4, xm(RCPU, OFF_ATTN), 0);
    jcc(t, XC_NE, t->entry_out);
    for (int r = 0; r < 16; r++)
        if (t->used & (1u << r))
            x_load(b, 4, H(t, r), xm(RCPU, OFF_REG(r)));
    bind(t, t->loop);

    for (unsigned i = 0; i < n; i++)
        emit_insn(t, i);
    if (!t->in[n - 1].ends_block)
        exit_chain(t, t->in[n - 1].pc + t->in[n - 1].len);

    emit_deferred(t);
    for (unsigned i = 0; i < n; i++)
        if (t->hostp[i] >= 0)
            emit_host_path(t, i);

    /* The bail exits: PC of the instruction, what came before counted. */
    for (unsigned i = 0; i < n; i++) {
        if (t->bail[i] < 0)
            continue;
        bind(t, t->bail[i]);
        t->pend.mode = PM_NONE;
        t->pend.mask = 0;
        t->own = t->own_at[i];
        flags_exit(t);
        x_mov_ri(b, RAX, t->in[i].pc);
        count(t, i);
        x_mov_ri(b, RCX, JIT_EXIT_BAIL);
        jmp(t, t->tail);
    }

    /* Not entered: nothing done. */
    bind(t, t->entry_out);
    x_mov_ri(b, RAX, t->in[0].pc);
    x_mov_ri(b, RCX, JIT_EXIT_NORMAL);
    jmp_abs(t, j->exit_off);

    /* The common tail: EAX the next PC, ECX the exit code. */
    bind(t, t->tail);
    exit_stores(t);
    jmp_abs(t, j->exit_off);

    const uint8_t *code = NULL;
    if (!b->overflow && !t->failed) {
        for (unsigned k = 0; k < t->nfix; k++)
            x_link(b, t->fix[k].at, t->label_pos[t->fix[k].label]);
        code = j->mem.rx + j->used;
        j->used += (b->pos + 15) & ~(size_t)15;
        if (j->used > j->mem.size)
            j->used = j->mem.size;
    }
    free(t);
    return code;
}

/* ---- The trampoline and the shared stubs ------------------------------ */

static void align16(xbuf *b)
{
    while (b->pos & 15)
        x_b(b, 0xCC);
}

/* enter(cpu, mem, code): saves the callee-saved registers, sets RBX and
 * R15 and jumps to the block. The common exit stores the PC and returns
 * ECX | EDX << 32. The check stub calls jit_access_check() with the
 * address in ECX and the size and direction pushed before the call, keeps
 * every register but RCX, and returns the answer in ECX. */
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
    /* RSP 16-byte aligned inside blocks (an even number of pushes) */
    x_rsp_adjust(b, -8);
#if defined(_WIN32)
    x_mov_rr(b, 8, RCPU, RCX);
    x_mov_rr(b, 8, RMEM, RDX);
    x_movzx_load(b, 2, RSR, xm(RCPU, OFF_SR));
    x_jmp_r(b, R8);
#else
    x_mov_rr(b, 8, RCPU, RDI);
    x_mov_rr(b, 8, RMEM, RSI);
    x_movzx_load(b, 2, RSR, xm(RCPU, OFF_SR));
    x_jmp_r(b, RDX);
#endif

    align16(b);
    size_t epilogue = b->pos;
    x_rsp_adjust(b, 8);
    for (int i = nsaved; i-- > 0;)
        x_pop(b, saved[i]);
    x_ret(b);

    align16(b);
    j->exit_off = b->pos;
    x_store(b, 4, xm(RCPU, OFF_PC), RAX);
    x_store(b, 2, xm(RCPU, OFF_SR), RSR);
    x_mov_rr(b, 4, RAX, RCX);
    x_shift_r(b, XS_SHL, 8, RDX, 32);
    x_alu_rr(b, XA_OR, 8, RAX, RDX);
    size_t at = x_jmp32(b);
    x_patch32(b, at, (uint32_t)((int64_t)epilogue - (int64_t)(at + 4)));

    align16(b);
    j->check_off = b->pos;
    static const int keep[] = { RAX, RDX, RSI, RDI, R8, R9, R10, R11, RBP };
    for (int i = 0; i < 9; i++)
        x_push(b, keep[i]);
    x_mov_rr(b, 8, RBP, RSP);
    x_alu_ri(b, XA_AND, 8, RSP, 0xFFFFFFF0u);
    int (*fn)(relevo_cpu *, uint32_t, uint32_t) = jit_access_check;
    uint64_t addr;
    memcpy(&addr, &fn, sizeof addr);
#if defined(_WIN32)
    x_rsp_adjust(b, -32);
    x_mov_rr(b, 4, RDX, RCX);
    x_mov_rr(b, 8, RCX, RCPU);
    x_load(b, 4, R8, xm(RBP, 80));
#else
    x_mov_rr(b, 8, RDI, RCPU);
    x_mov_rr(b, 4, RSI, RCX);
    x_load(b, 4, RDX, xm(RBP, 80));
#endif
    x_mov_ri64(b, RAX, addr);
    x_call_r(b, RAX);
    x_mov_rr(b, 4, RCX, RAX);
    x_mov_rr(b, 8, RSP, RBP);
    for (int i = 9; i-- > 0;)
        x_pop(b, keep[i]);
    x_ret(b);

    /* The record merge: ECX the CCR bits to keep in R14. Takes N Z V C
     * (those the record gives) and X from the record, and empties it.
     * Changes RAX, RCX, RDX and the flags. */
    align16(b);
    j->ccr_off = b->pos;
    {
        x_push(b, RSI);
        x_push(b, RDI);
        x_mov_rr(b, 4, RSI, RCX);
        x_unary(b, XU_NOT, 4, RSI);                 /* ESI: what to take */
        x_movzx_load(b, 2, RAX, xm(RCPU, OFF_CCR));
        x_mov_rr(b, 4, RCX, RAX);                   /* N Z: bits 15, 14 */
        x_shift_ri(b, 5, RCX, 12);
        x_alu_ri(b, XA_AND, 4, RCX, JF_N | JF_Z);
        x_mov_rr(b, 4, RDX, RAX);                   /* V: bit 0 */
        x_alu_ri(b, XA_AND, 4, RDX, 1);
        x_alu_rr(b, XA_ADD, 4, RDX, RDX);
        x_alu_rr(b, XA_OR, 4, RCX, RDX);
        x_mov_rr(b, 4, RDX, RAX);                   /* C: bit 8 */
        x_shift_ri(b, 5, RDX, 8);
        x_alu_ri(b, XA_AND, 4, RDX, 1);
        x_alu_rr(b, XA_OR, 4, RCX, RDX);
        x_movzx_load(b, 1, RDI, xm(RCPU, OFF_CCR + 2));
        x_alu_rr(b, XA_AND, 4, RDI, RSI);           /* EDI: bits taken */
        x_movzx_load(b, 1, RAX, xm(RCPU, OFF_CCR + 3));
        x_test_rr(b, 4, RAX, RAX);
        size_t no_x = x_jcc32(b, XC_E);
        x_test_ri(b, 4, RSI, JF_X);
        size_t no_x2 = x_jcc32(b, XC_E);
        x_alu_ri(b, XA_AND, 4, RAX, 1);
        x_shift_ri(b, 4, RAX, 4);
        x_alu_rr(b, XA_OR, 4, RCX, RAX);
        x_alu_ri(b, XA_OR, 4, RDI, JF_X);
        x_link(b, no_x, (int64_t)b->pos);
        x_link(b, no_x2, (int64_t)b->pos);
        x_alu_rr(b, XA_AND, 4, RCX, RDI);
        x_unary(b, XU_NOT, 4, RDI);
        x_alu_rr(b, XA_AND, 4, RSR, RDI);
        x_alu_rr(b, XA_OR, 4, RSR, RCX);
        x_store_imm(b, 2, xm(RCPU, OFF_CCR + 2), 0);
        x_pop(b, RDI);
        x_pop(b, RSI);
        x_ret(b);
    }

    if (b->overflow)
        return false;
    j->enter = j->mem.rx;
    j->used = j->base_used = (b->pos + 15) & ~(size_t)15;
    return true;
}

typedef uint64_t (*enter_fn)(relevo_cpu *, uint8_t *, const uint8_t *);

/* Clang's -fsanitize=function checks a signature stored before the target
 * of an indirect call; generated code has none. */
#if defined(__clang__) && __clang_major__ >= 17
__attribute__((no_sanitize("function")))
#endif
uint64_t jit_backend_enter(relevo_cpu *c, jit_state *j, const uint8_t *code)
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

bool jit_backend_has_lahf(void)
{
#if defined(__GNUC__)
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000001u, &a, &b, &c, &d))
        return false;
    return c & 1;
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

bool jit_backend_has_lahf(void)
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

uint64_t jit_backend_enter(relevo_cpu *c, jit_state *j, const uint8_t *code)
{
    (void)c;
    (void)j;
    (void)code;
    return 1;
}

#endif
