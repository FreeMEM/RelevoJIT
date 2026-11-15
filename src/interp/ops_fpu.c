/* SPDX-License-Identifier: MPL-2.0
 *
 * The floating-point unit: the MC68881/MC68882 next to a 68020 or 68030,
 * and the FPU built into the 68040 and 68060 (MC68881UM, M68000PRM chapter
 * 5, MC68040UM chapter 9, MC68060UM chapter 6). Coprocessor 1 of line F:
 *
 *   F200+ea  general: arithmetic, FMOVE in and out, FMOVECR, FMOVEM
 *   F240+ea  FScc, FDBcc (F248+n), FTRAPcc (F27A-F27C)
 *   F280/F2C0  FBcc with a 16 or 32-bit displacement (FNOP is FBF.W 0)
 *   F300+ea  FSAVE, F340+ea FRESTORE (privileged)
 *
 * The arithmetic is in ../fpx.c. Each instruction that computes clears the
 * exception byte of the FPSR, sets the bits it raises and ORs them into the
 * accrued byte. FPIAR gets the address of the instructions that can raise
 * an exception: the general ones but FMOVEM, and the conditional ones
 * (BSUN).
 *
 * Exceptions: an exception raised and enabled in the FPCR is taken, as on
 * the 68881, before the next floating-point instruction (other than FSAVE,
 * FRESTORE and the moves of control registers): a format $0 frame with the
 * address of that instruction, through vector 48 (BSUN) to 54 (SNAN), the
 * highest priority first. With SNAN, OPERR or DZ enabled the destination
 * is left as it was; with OVFL, UNFL or INEX it gets the normal result.
 * BSUN (a predicate 0x10-0x1F that meets a NaN) is taken at once, before
 * the conditional instruction runs. FSAVE puts a pending exception in the
 * frame and FRESTORE takes it back.
 *
 * The 68040 and 68060 leave the transcendental functions, FMOVECR, FGETEXP,
 * FGETMAN, FMOD, FREM, FSCALE, FSGLMUL and FSGLDIV (and FINT, FINTRZ on the
 * 68040; FScc, FDBcc and FTRAPcc on the 68060) to software: line F, vector
 * 11, as any unimplemented instruction (the host's line F function first).
 * The packed decimal format and denormalized operands, also software on
 * those chips, are executed here.
 *
 * Built without libm (RELEVO_NO_LIBM, make freestanding), every FPU sends
 * the transcendental functions to line F that way.
 */
#include "interp.h"
#include "../fpx.h"

#if defined(RELEVO_NO_LIBM)
#define HAVE_TRANS 0
#else
#define HAVE_TRANS 1
#endif

enum {
    VEC_FP_BSUN = 48,
    VEC_FP_INEX = 49,
    VEC_FP_DZ = 50,
    VEC_FP_UNFL = 51,
    VEC_FP_OPERR = 52,
    VEC_FP_OVFL = 53,
    VEC_FP_SNAN = 54
};

#define FPCR_MASK 0x0000FFF0u
#define FPSR_MASK 0x0FFFFFF8u
#define FPSR_CC   0x0F000000u
#define FPSR_QUOT 0x00FF0000u
#define FPSR_EXC  0x0000FF00u

static bool internal_fpu(const relevo_cpu *c)
{
    return c->fpu == RELEVO_FPU_INTERNAL;
}

void fpu_reset(relevo_cpu *c)
{
    for (unsigned i = 0; i < 8; i++)
        c->r.fp[i] = fpx_default_nan();
    c->r.fpcr = 0;
    c->r.fpsr = 0;
    c->r.fpiar = 0;
    c->fpu_null = true;
    c->fpu_pending = 0;
}

/* An encoding this FPU does not execute: line F, as if the instruction
 * had not started (the host sees the PC after the opcode). */
static void fline(relevo_cpu *c, uint16_t op)
{
    c->r.pc = c->insn_pc + 2;
    op_line_f(c, op);
}

/* Every floating-point instruction but FSAVE and FRESTORE starts here.
 * Takes an exception left pending by the previous one (when `pending` is
 * true); false if it did. */
static bool fpu_begin(relevo_cpu *c, bool pending)
{
    c->fpu_null = false;
    if (pending && UNLIKELY(c->fpu_pending != 0)) {
        unsigned v = c->fpu_pending;
        c->fpu_pending = 0;
        cpu_exception_group1(c, v);
        return false;
    }
    return true;
}

static fpx_env fpu_env(const relevo_cpu *c)
{
    static const unsigned precs[4] = { 64, 24, 53, 64 };
    fpx_env e;
    e.mode = (c->r.fpcr >> 4) & 3;
    e.prec = precs[(c->r.fpcr >> 6) & 3];
    e.flags = 0;
    return e;
}

/* The exception vector for enabled exceptions, by priority. */
static unsigned trap_vector(unsigned en)
{
    if (en & FPX_BSUN) return VEC_FP_BSUN;
    if (en & FPX_SNAN) return VEC_FP_SNAN;
    if (en & FPX_OPERR) return VEC_FP_OPERR;
    if (en & FPX_OVFL) return VEC_FP_OVFL;
    if (en & FPX_UNFL) return VEC_FP_UNFL;
    if (en & FPX_DZ) return VEC_FP_DZ;
    return VEC_FP_INEX;
}

/* The end of an instruction that computes: exception and accrued bytes,
 * an enabled exception left pending. Returns whether the destination may
 * be written. */
static bool fpu_status(relevo_cpu *c, unsigned flags)
{
    c->r.fpsr = (c->r.fpsr & ~FPSR_EXC) | (flags & FPSR_EXC)
              | fpx_accrued(flags);
    unsigned en = flags & c->r.fpcr & 0xFF00;
    if (en != 0)
        c->fpu_pending = trap_vector(en);
    return !(en & (FPX_SNAN | FPX_OPERR | FPX_DZ));
}

static void set_cc(relevo_cpu *c, uint32_t cc)
{
    c->r.fpsr = (c->r.fpsr & ~FPSR_CC) | cc;
}

/* ---- Operands in memory ----------------------------------------------- */

/* Formats of the source/destination specifier: L S X P W D B (7: packed
 * with a dynamic k-factor on output, FMOVECR on input). */
enum { F_L, F_S, F_X, F_P, F_W, F_D, F_B, F_PDYN };
static const unsigned fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };

/* Reads `size` bytes (1, 2, 4, 8, 12) at addr into w[] as longs (a byte or
 * word in the low bits of w[0]). */
static void read_mem(relevo_cpu *c, uint32_t addr, unsigned size,
                     unsigned acc, uint32_t w[3])
{
    if (size == 1) {
        w[0] = cpu_read8(c, addr, acc);
    } else if (size == 2) {
        w[0] = cpu_read16(c, addr, acc);
    } else {
        for (unsigned i = 0; i < size / 4; i++)
            w[i] = cpu_read32(c, addr + 4 * i, acc);
    }
}

static void write_mem(relevo_cpu *c, uint32_t addr, unsigned size,
                      const uint32_t w[3])
{
    if (size == 1) {
        cpu_write8(c, addr, w[0]);
    } else if (size == 2) {
        cpu_write16(c, addr, w[0]);
    } else {
        for (unsigned i = 0; i < size / 4; i++)
            cpu_write32(c, addr + 4 * i, w[i]);
    }
}

/* Whether mode/reg can be the source of format fmt: Dn only for the
 * formats of 4 bytes or less but not packed, no An. */
static bool src_valid(unsigned mode, unsigned reg, unsigned fmt)
{
    if (mode == 0)
        return fmt == F_L || fmt == F_S || fmt == F_W || fmt == F_B;
    if (mode == 1)
        return false;
    return mode < 7 || reg <= 4;
}

/* Data alterable for a destination; Dn for the short formats. */
static bool dst_valid(unsigned mode, unsigned reg, unsigned fmt)
{
    if (mode == 0)
        return fmt == F_L || fmt == F_S || fmt == F_W || fmt == F_B;
    if (mode == 1)
        return false;
    return mode < 7 || reg <= 1;
}

/* Fetches the raw source operand of the instruction (op's EA). */
static void fetch_operand(relevo_cpu *c, uint16_t op, unsigned size,
                          uint32_t w[3], uint32_t *ea_addr)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    *ea_addr = 0;
    if (mode == 0) {
        w[0] = c->r.d[reg] & sz_mask(size);
        return;
    }
    if (mode == 7 && reg == 4) {
        /* #imm: a byte is in the low half of a word */
        if (size <= 2) {
            w[0] = fetch16(c) & sz_mask(size);
        } else {
            for (unsigned i = 0; i < size / 4; i++)
                w[i] = fetch32(c);
        }
        return;
    }
    ea_t e;
    ea_decode(c, &e, mode, reg, size);
    *ea_addr = e.addr;
    read_mem(c, e.addr, size, e.acc, w);
    ea_commit(c, &e);
}

/* The source converted to extended (exact, but for packed decimal). */
static fpx convert_in(relevo_cpu *c, unsigned fmt, const uint32_t w[3],
                      unsigned *flags)
{
    fpx_env e = fpu_env(c);
    e.prec = 64;
    fpx v;
    switch (fmt) {
    case F_L: v = fpx_from_int(&e, (int32_t)w[0]); break;
    case F_W: v = fpx_from_int(&e, (int32_t)sext(w[0], SZ_W)); break;
    case F_B: v = fpx_from_int(&e, (int32_t)sext(w[0], SZ_B)); break;
    case F_S: v = fpx_from_single(w[0]); break;
    case F_D: v = fpx_from_double((uint64_t)w[0] << 32 | w[1]); break;
    case F_X: v = fpx_make(w[0] >> 31, (w[0] >> 16) & 0x7FFF,
                           (uint64_t)w[1] << 32 | w[2]); break;
    default:  v = fpx_from_packed(&e, w); break;
    }
    *flags |= e.flags;
    return v;
}

/* ---- Arithmetic --------------------------------------------------------- */

/* What an opmode (command bits 6-0) is. */
enum {
    K_NONE,         /* not an instruction: line F */
    K_MONADIC,
    K_DYADIC,
    K_TRANS,        /* transcendental (fpx_trans) */
    K_SINCOS,
    K_CMP,
    K_TST
};

typedef struct opinfo {
    uint8_t kind;
    uint8_t func;       /* fpx_func for K_TRANS */
    uint8_t soft040;    /* left to software by the 68040 (2: also 68060) */
} opinfo;

static opinfo opmode_info(unsigned m)
{
    static const struct { uint8_t mode, kind, func, soft; } t[] = {
        { 0x00, K_MONADIC, 0, 0 },          /* FMOVE */
        { 0x01, K_MONADIC, 0, 1 },          /* FINT */
        { 0x02, K_TRANS, FPX_SINH, 2 },
        { 0x03, K_MONADIC, 0, 1 },          /* FINTRZ */
        { 0x04, K_MONADIC, 0, 0 },          /* FSQRT */
        { 0x06, K_TRANS, FPX_LOGNP1, 2 },
        { 0x08, K_TRANS, FPX_ETOXM1, 2 },
        { 0x09, K_TRANS, FPX_TANH, 2 },
        { 0x0A, K_TRANS, FPX_ATAN, 2 },
        { 0x0C, K_TRANS, FPX_ASIN, 2 },
        { 0x0D, K_TRANS, FPX_ATANH, 2 },
        { 0x0E, K_TRANS, FPX_SIN, 2 },
        { 0x0F, K_TRANS, FPX_TAN, 2 },
        { 0x10, K_TRANS, FPX_ETOX, 2 },
        { 0x11, K_TRANS, FPX_TWOTOX, 2 },
        { 0x12, K_TRANS, FPX_TENTOX, 2 },
        { 0x14, K_TRANS, FPX_LOGN, 2 },
        { 0x15, K_TRANS, FPX_LOG10, 2 },
        { 0x16, K_TRANS, FPX_LOG2, 2 },
        { 0x18, K_MONADIC, 0, 0 },          /* FABS */
        { 0x19, K_TRANS, FPX_COSH, 2 },
        { 0x1A, K_MONADIC, 0, 0 },          /* FNEG */
        { 0x1C, K_TRANS, FPX_ACOS, 2 },
        { 0x1D, K_TRANS, FPX_COS, 2 },
        { 0x1E, K_MONADIC, 0, 2 },          /* FGETEXP */
        { 0x1F, K_MONADIC, 0, 2 },          /* FGETMAN */
        { 0x20, K_DYADIC, 0, 0 },           /* FDIV */
        { 0x21, K_DYADIC, 0, 2 },           /* FMOD */
        { 0x22, K_DYADIC, 0, 0 },           /* FADD */
        { 0x23, K_DYADIC, 0, 0 },           /* FMUL */
        { 0x24, K_DYADIC, 0, 2 },           /* FSGLDIV */
        { 0x25, K_DYADIC, 0, 2 },           /* FREM */
        { 0x26, K_DYADIC, 0, 2 },           /* FSCALE */
        { 0x27, K_DYADIC, 0, 2 },           /* FSGLMUL */
        { 0x28, K_DYADIC, 0, 0 },           /* FSUB */
        { 0x38, K_CMP, 0, 0 },
        { 0x3A, K_TST, 0, 0 },
    };
    opinfo r = { K_NONE, 0, 0 };
    if (m >= 0x30 && m <= 0x37) {
        r.kind = K_SINCOS;
        r.soft040 = 2;
        return r;
    }
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].mode == m) {
            r.kind = t[i].kind;
            r.func = t[i].func;
            r.soft040 = t[i].soft;
        }
    return r;
}

/* The 68040's single and double precision forms: the opmode they round
 * and their precision, or 0. */
static unsigned sd_form(unsigned m, unsigned *base)
{
    static const uint8_t t[][3] = {
        { 0x40, 0x00, 24 }, { 0x41, 0x04, 24 }, { 0x44, 0x00, 53 },
        { 0x45, 0x04, 53 }, { 0x58, 0x18, 24 }, { 0x5A, 0x1A, 24 },
        { 0x5C, 0x18, 53 }, { 0x5E, 0x1A, 53 }, { 0x60, 0x20, 24 },
        { 0x62, 0x22, 24 }, { 0x63, 0x23, 24 }, { 0x64, 0x20, 53 },
        { 0x66, 0x22, 53 }, { 0x67, 0x23, 53 }, { 0x68, 0x28, 24 },
        { 0x6C, 0x28, 53 },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i][0] == m) {
            *base = t[i][1];
            return t[i][2];
        }
    return 0;
}

static fpx monadic(fpx_env *e, unsigned m, fpx s)
{
    switch (m) {
    case 0x00: return fpx_round(e, s);
    case 0x01: return fpx_int(e, s, e->mode);
    case 0x03: return fpx_int(e, s, FPX_RZ);
    case 0x04: return fpx_sqrt(e, s);
    case 0x18: return fpx_abs(e, s);
    case 0x1A: return fpx_neg(e, s);
    case 0x1E: return fpx_getexp(e, s);
    default:   return fpx_getman(e, s);
    }
}

static fpx dyadic(relevo_cpu *c, fpx_env *e, unsigned m, fpx d, fpx s)
{
    uint8_t q;
    fpx r;
    switch (m) {
    case 0x20: return fpx_div(e, d, s);
    case 0x22: return fpx_add(e, d, s);
    case 0x23: return fpx_mul(e, d, s);
    case 0x24: e->prec = 24; return fpx_div(e, d, s);
    case 0x26: return fpx_scale(e, d, s);
    case 0x27: e->prec = 24; return fpx_mul(e, d, s);
    case 0x28: return fpx_sub(e, d, s);
    default:
        r = fpx_rem(e, d, s, m == 0x25, &q);
        c->r.fpsr = (c->r.fpsr & ~FPSR_QUOT) | (uint32_t)q << 16;
        return r;
    }
}

/* Opclasses 000 (FPm to FPn) and 010 (<ea> to FPn, FMOVECR). */
static void op_arith(relevo_cpu *c, uint16_t op, uint16_t cmd)
{
    unsigned m = cmd & 0x7F, dst = (cmd >> 7) & 7, spec = (cmd >> 10) & 7;
    bool from_ea = cmd & 0x4000;
    unsigned mode = (op >> 3) & 7, reg = op & 7;

    if (from_ea && spec == 7) {             /* FMOVECR */
        fpx_env e = fpu_env(c);
        fpx v;
        if (internal_fpu(c) || (op & 0x3F) != 0 || !fpx_constant(&e, m, &v)) {
            fline(c, op);
            return;
        }
        if (!fpu_begin(c, true))
            return;
        c->r.fpiar = c->insn_pc;
        if (fpu_status(c, e.flags)) {
            c->r.fp[dst] = v;
            set_cc(c, fpx_cc(v));
        }
        return;
    }

    unsigned base = m, prec = 0;
    if (m >= 0x40) {
        prec = internal_fpu(c) ? sd_form(m, &base) : 0;
        if (prec == 0) {
            fline(c, op);
            return;
        }
    }
    opinfo info = opmode_info(base);
    if (info.kind == K_NONE || (from_ea && !src_valid(mode, reg, spec))
        || (!HAVE_TRANS && (info.kind == K_TRANS || info.kind == K_SINCOS))
        || (internal_fpu(c) && info.soft040
            && (c->model == RELEVO_68040 || info.soft040 == 2))) {
        fline(c, op);
        return;
    }
    if (!fpu_begin(c, true))
        return;
    c->r.fpiar = c->insn_pc;

    unsigned flags = 0;
    fpx src;
    if (from_ea) {
        uint32_t w[3] = { 0, 0, 0 }, addr;
        fetch_operand(c, op, fmt_size[spec], w, &addr);
        src = convert_in(c, spec, w, &flags);
    } else {
        src = c->r.fp[spec];
    }

    fpx_env e = fpu_env(c);
    if (prec)
        e.prec = prec;
    fpx res;
    switch (info.kind) {
    case K_TST:
        if (fpx_is_snan(src))
            flags |= FPX_SNAN;
        fpu_status(c, flags);
        set_cc(c, fpx_cc(src));
        return;
    case K_CMP: {
        uint32_t cc = fpx_cmp(&e, c->r.fp[dst], src);
        fpu_status(c, flags | e.flags);
        set_cc(c, cc);
        return;
    }
    case K_SINCOS: {
        fpx_env e2 = e;
        fpx cosv = fpx_trans(&e2, FPX_COS, src);
        res = fpx_trans(&e, FPX_SIN, src);
        if (fpu_status(c, flags | e.flags | e2.flags)) {
            c->r.fp[m & 7] = cosv;
            c->r.fp[dst] = res;
            set_cc(c, fpx_cc(res));
        }
        return;
    }
    case K_TRANS:
        res = fpx_trans(&e, (fpx_func)info.func, src);
        break;
    case K_MONADIC:
        res = monadic(&e, base, src);
        break;
    default:
        res = dyadic(c, &e, base, c->r.fp[dst], src);
        break;
    }
    if (fpu_status(c, flags | e.flags)) {
        c->r.fp[dst] = res;
        set_cc(c, fpx_cc(res));
    }
}

/* Opclass 011: FMOVE FPm,<ea>. */
static void op_fmove_out(relevo_cpu *c, uint16_t op, uint16_t cmd)
{
    unsigned fmt = (cmd >> 10) & 7, src = (cmd >> 7) & 7;
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    if (!dst_valid(mode, reg, fmt)) {
        fline(c, op);
        return;
    }
    if (!fpu_begin(c, true))
        return;
    c->r.fpiar = c->insn_pc;
    unsigned size = fmt_size[fmt];
    ea_t ea;
    if (mode != 0)
        ea_decode(c, &ea, mode, reg, size);

    fpx_env e = fpu_env(c);
    fpx v = c->r.fp[src];
    uint32_t w[3] = { 0, 0, 0 };
    switch (fmt) {
    case F_L: w[0] = (uint32_t)fpx_to_int(&e, v, 4); break;
    case F_W: w[0] = (uint32_t)fpx_to_int(&e, v, 2) & 0xFFFF; break;
    case F_B: w[0] = (uint32_t)fpx_to_int(&e, v, 1) & 0xFF; break;
    case F_S: w[0] = fpx_to_single(&e, v); break;
    case F_D: {
        uint64_t d = fpx_to_double(&e, v);
        w[0] = (uint32_t)(d >> 32);
        w[1] = (uint32_t)d;
        break;
    }
    case F_X: {
        fpx x = fpx_to_extended(&e, v);
        w[0] = (uint32_t)x.sign_exponent << 16;
        w[1] = (uint32_t)(x.mantissa >> 32);
        w[2] = (uint32_t)x.mantissa;
        break;
    }
    default: {
        /* the k-factor: 7 bits with sign, static or in Dn */
        uint32_t kreg = fmt == F_P ? cmd : c->r.d[(cmd >> 4) & 7];
        int k = (int)(kreg & 0x7F);
        if (k & 0x40)
            k -= 128;
        fpx_to_packed(&e, v, k, w);
        break;
    }
    }
    if (!fpu_status(c, e.flags))
        return;
    if (mode == 0) {
        set_dreg(c, reg, w[0], size);
        return;
    }
    write_mem(c, ea.addr, size, w);
    ea_commit(c, &ea);
}

/* ---- FMOVEM -------------------------------------------------------------- */

static unsigned popcount8(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

/* Opclasses 100 and 101: the control registers (bit 12 FPCR, 11 FPSR, 10
 * FPIAR) from or to <ea>, in that order in memory. */
static void op_fmovem_ctrl(relevo_cpu *c, uint16_t op, uint16_t cmd)
{
    bool to_ea = cmd & 0x2000;
    unsigned list = (cmd >> 10) & 7, n = popcount8(list);
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    bool ok = n != 0;
    if (mode == 0)
        ok = ok && n == 1;
    else if (mode == 1)
        ok = ok && list == 1;
    else if (mode == 7 && reg >= 2)
        ok = ok && !to_ea && (reg <= 3 || reg == 4);
    if (mode == 7 && reg > 4)
        ok = false;
    if (!ok) {
        fline(c, op);
        return;
    }
    fpu_begin(c, false);
    uint32_t *regs[3] = { &c->r.fpcr, &c->r.fpsr, &c->r.fpiar };
    static const uint32_t masks[3] = { FPCR_MASK, FPSR_MASK, 0xFFFFFFFFu };

    if (mode <= 1) {
        for (unsigned i = 0; i < 3; i++) {
            if (!(list & (4 >> i)))
                continue;
            uint32_t *rn = mode == 0 ? &c->r.d[reg] : &c->r.a[reg];
            if (to_ea)
                *rn = *regs[i];
            else
                *regs[i] = *rn & masks[i];
        }
        return;
    }
    uint32_t addr;
    ea_t e;
    bool imm = mode == 7 && reg == 4;
    if (mode == 4) {
        addr = c->r.a[reg] - 4 * n;
        c->r.a[reg] = addr;
    } else if (mode == 3) {
        addr = c->r.a[reg];
        c->r.a[reg] += 4 * n;
    } else if (imm) {
        addr = 0;
    } else {
        ea_decode(c, &e, mode, reg, SZ_L);
        addr = e.addr;
    }
    unsigned acc = (mode == 7 && (reg == 2 || reg == 3)) ? ACC_PROG : ACC_DATA;
    for (unsigned i = 0; i < 3; i++) {
        if (!(list & (4 >> i)))
            continue;
        if (to_ea) {
            cpu_write32(c, addr, *regs[i]);
        } else {
            uint32_t v = imm ? fetch32(c) : cpu_read32(c, addr, acc);
            *regs[i] = v & masks[i];
        }
        addr += 4;
    }
}

/* Opclasses 110 and 111: FMOVEM.X of data registers. Bit 11 of the
 * command: list in Dn; bit 12: postincrement or control mode (list bit 7
 * is FP0) rather than predecrement (bit 0 is FP0). FP0 is always at the
 * lowest address. */
static void op_fmovem_x(relevo_cpu *c, uint16_t op, uint16_t cmd)
{
    bool to_ea = cmd & 0x2000, postinc = cmd & 0x1000;
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    bool ok;
    if (to_ea)
        ok = postinc ? (mode == 2 || mode == 5 || mode == 6
                        || (mode == 7 && reg <= 1))
                     : mode == 4;
    else
        ok = postinc && (mode == 2 || mode == 3 || mode == 5 || mode == 6
                         || (mode == 7 && reg <= 3));
    if (!ok) {
        fline(c, op);
        return;
    }
    if (!fpu_begin(c, true))
        return;
    unsigned list = (cmd & 0x0800) ? c->r.d[(cmd >> 4) & 7] & 0xFF
                                   : cmd & 0xFF;
    unsigned n = popcount8(list);
    uint32_t addr;
    unsigned acc = ACC_DATA;
    if (mode == 4) {
        addr = c->r.a[reg] - 12 * n;
    } else if (mode == 3) {
        addr = c->r.a[reg];
    } else {
        ea_t e;
        ea_decode(c, &e, mode, reg, SZ_L);
        addr = e.addr;
        acc = e.acc;
    }
    uint32_t a0 = addr;
    for (unsigned i = 0; i < 8; i++) {
        unsigned bit = postinc ? 0x80u >> i : 1u << i;
        if (!(list & bit))
            continue;
        uint32_t w[3];
        if (to_ea) {
            w[0] = (uint32_t)c->r.fp[i].sign_exponent << 16;
            w[1] = (uint32_t)(c->r.fp[i].mantissa >> 32);
            w[2] = (uint32_t)c->r.fp[i].mantissa;
            write_mem(c, addr, 12, w);
        } else {
            read_mem(c, addr, 12, acc, w);
            c->r.fp[i].sign_exponent = (uint16_t)(w[0] >> 16);
            c->r.fp[i].mantissa = (uint64_t)w[1] << 32 | w[2];
        }
        addr += 12;
    }
    if (mode == 4)
        c->r.a[reg] = a0;
    else if (mode == 3)
        c->r.a[reg] = addr;
}

static void op_fgen(relevo_cpu *c, uint16_t op)
{
    uint16_t cmd = (uint16_t)fetch16(c);
    switch (cmd >> 13) {
    case 0:
    case 2:
        op_arith(c, op, cmd);
        return;
    case 3:
        op_fmove_out(c, op, cmd);
        return;
    case 4:
    case 5:
        op_fmovem_ctrl(c, op, cmd);
        return;
    case 6:
    case 7:
        op_fmovem_x(c, op, cmd);
        return;
    default:
        fline(c, op);
        return;
    }
}

/* ---- Conditions ------------------------------------------------------- */

/* Evaluates a conditional predicate (MC68881UM 4.4). Predicates 0x10 to
 * 0x1F set BSUN when NAN is set, and take its exception at once if
 * enabled; *taken is then true. */
static bool fp_cond(relevo_cpu *c, unsigned pred, bool *taken)
{
    uint32_t cc = c->r.fpsr;
    bool n = cc & FPX_CC_N, z = cc & FPX_CC_Z, nan = cc & FPX_CC_NAN;
    *taken = false;
    if ((pred & 0x10) && nan) {
        c->r.fpsr |= FPX_BSUN | 0x80;
        if (c->r.fpcr & FPX_BSUN) {
            cpu_exception_group1(c, VEC_FP_BSUN);
            *taken = true;
            return false;
        }
    }
    switch (pred & 0xF) {
    case 0x0: return false;                         /* F, SF */
    case 0x1: return z;                             /* EQ, SEQ */
    case 0x2: return !(nan || z || n);              /* OGT, GT */
    case 0x3: return z || !(nan || n);              /* OGE, GE */
    case 0x4: return n && !(nan || z);              /* OLT, LT */
    case 0x5: return z || (n && !nan);              /* OLE, LE */
    case 0x6: return !(nan || z);                   /* OGL, GL */
    case 0x7: return !nan;                          /* OR, GLE */
    case 0x8: return nan;                           /* UN, NGLE */
    case 0x9: return nan || z;                      /* UEQ, NGL */
    case 0xA: return nan || !(n || z);              /* UGT, NLE */
    case 0xB: return nan || z || !n;                /* UGE, NLT */
    case 0xC: return nan || (n && !z);              /* ULT, NGE */
    case 0xD: return nan || z || n;                 /* ULE, NGT */
    case 0xE: return !z;                            /* NE, SNE */
    default:  return true;                          /* T, ST */
    }
}

/* FBcc (F280 + predicate: 16-bit displacement, F2C0: 32-bit). */
static void op_fbcc(relevo_cpu *c, uint16_t op)
{
    if ((op & 0x20) != 0) {
        fline(c, op);
        return;
    }
    if (!fpu_begin(c, true))
        return;
    c->r.fpiar = c->insn_pc;
    uint32_t base = c->r.pc;
    uint32_t disp = (op & 0x40) ? fetch32(c) : sext(fetch16(c), SZ_W);
    bool taken;
    bool t = fp_cond(c, op & 0x3F, &taken);
    if (!taken && t)
        jump(c, base + disp, c->r.pc);
}

/* F240 + ea: FScc; F248 + n: FDBcc; F27A/F27B/F27C: FTRAPcc. The
 * predicate is in the extension word. */
static void op_fcc_misc(relevo_cpu *c, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    if (c->model == RELEVO_68060 || (mode == 7 && reg > 4)) {
        fline(c, op);
        return;
    }
    uint16_t ext = (uint16_t)fetch16(c);
    if (ext & 0xFFE0) {
        fline(c, op);
        return;
    }
    if (!fpu_begin(c, true))
        return;
    c->r.fpiar = c->insn_pc;
    bool taken;
    if (mode == 1) {                        /* FDBcc */
        uint32_t base = c->r.pc;
        uint32_t disp = sext(fetch16(c), SZ_W);
        bool t = fp_cond(c, ext, &taken);
        if (taken || t)
            return;
        uint32_t n = (c->r.d[reg] - 1) & 0xFFFF;
        set_dreg(c, reg, n, SZ_W);
        if (n != 0xFFFF)
            jump(c, base + disp, c->r.pc);
        return;
    }
    if (mode == 7 && reg >= 2) {            /* FTRAPcc */
        if (reg == 2)
            fetch16(c);
        else if (reg == 3)
            fetch32(c);
        bool t = fp_cond(c, ext, &taken);
        if (!taken && t)
            cpu_exception(c, VEC_TRAPV, c->r.pc);
        return;
    }
    ea_t e;                                 /* FScc */
    ea_decode(c, &e, mode, reg, SZ_B);
    bool t = fp_cond(c, ext, &taken);
    if (!taken)
        ea_write(c, &e, t ? 0xFF : 0);
}

/* ---- FSAVE and FRESTORE ------------------------------------------------ */

/* The idle frame (MC68881UM 6.4, MC68040UM 9.6, MC68060UM 6.5): its first
 * long and its total size. The internal words are 0, but one byte keeps a
 * pending exception's vector. */
static unsigned idle_frame(const relevo_cpu *c, uint32_t w[16])
{
    for (unsigned i = 0; i < 16; i++)
        w[i] = 0;
    switch (c->fpu) {
    case RELEVO_FPU_68881:
        w[0] = 0x1F180000;
        w[6] = c->fpu_pending;
        return 28;
    case RELEVO_FPU_68882:
        w[0] = 0x1F380000;
        w[14] = c->fpu_pending;
        return 60;
    default:
        if (c->model == RELEVO_68060) {
            w[0] = 0x00006000;
            w[2] = c->fpu_pending;
            return 12;
        }
        w[0] = 0x41000000 | c->fpu_pending;
        return 4;
    }
}

static void op_fsave(relevo_cpu *c, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    if (!(mode == 2 || mode == 4 || mode == 5 || mode == 6
          || (mode == 7 && reg <= 1))) {
        fline(c, op);
        return;
    }
    if (!supervisor_or_trap(c))
        return;
    uint32_t w[16];
    unsigned size;
    if (c->fpu_null) {
        for (unsigned i = 0; i < 3; i++)
            w[i] = 0;
        size = c->model == RELEVO_68060 ? 12 : 4;
    } else {
        size = idle_frame(c, w);
    }
    uint32_t addr;
    if (mode == 4) {
        addr = c->r.a[reg] - size;
    } else {
        ea_t e;
        ea_decode(c, &e, mode, reg, SZ_L);
        addr = e.addr;
    }
    for (unsigned i = 0; i < size / 4; i++)
        cpu_write32(c, addr + 4 * i, w[i]);
    if (mode == 4)
        c->r.a[reg] = addr;
    c->fpu_pending = 0;
}

static void op_frestore(relevo_cpu *c, uint16_t op)
{
    unsigned mode = (op >> 3) & 7, reg = op & 7;
    if (!(mode == 2 || mode == 3 || mode == 5 || mode == 6
          || (mode == 7 && reg <= 3))) {
        fline(c, op);
        return;
    }
    if (!supervisor_or_trap(c))
        return;
    uint32_t addr;
    unsigned acc = ACC_DATA;
    if (mode == 3) {
        addr = c->r.a[reg];
    } else {
        ea_t e;
        ea_decode(c, &e, mode, reg, SZ_L);
        addr = e.addr;
        acc = e.acc;
    }
    uint32_t w0 = cpu_read32(c, addr, acc);
    unsigned size, pending = 0;
    bool null, valid;
    if (c->model == RELEVO_68060) {
        unsigned type = (w0 >> 8) & 0xFF;
        size = 12;
        null = type == 0;
        valid = null || type == 0x60 || type == 0xE0;
        if (valid && !null)
            pending = cpu_read32(c, addr + 8, acc) & 0xFF;
    } else {
        unsigned ver = w0 >> 24, len = (w0 >> 16) & 0xFF;
        size = 4 + len;
        null = ver == 0;
        switch (c->fpu) {
        case RELEVO_FPU_68881:
            valid = ver == 0x1F && (len == 0x18 || len == 0xB4);
            if (valid && len == 0x18)
                pending = cpu_read32(c, addr + 24, acc) & 0xFF;
            break;
        case RELEVO_FPU_68882:
            valid = ver == 0x1F && (len == 0x38 || len == 0xD4);
            if (valid && len == 0x38)
                pending = cpu_read32(c, addr + 56, acc) & 0xFF;
            break;
        default:
            valid = ver == 0x41 && (len == 0 || len == 0x30 || len == 0x60);
            if (valid && len == 0)
                pending = w0 & 0xFF;
            break;
        }
        if (null)
            size = 4;
    }
    if (!null && !valid) {
        cpu_exception_group1(c, VEC_FORMAT_ERROR);
        return;
    }
    if (null) {
        fpu_reset(c);
    } else {
        c->fpu_null = false;
        c->fpu_pending = pending >= VEC_FP_BSUN && pending <= VEC_FP_SNAN
                       ? pending : 0;
    }
    if (mode == 3)
        c->r.a[reg] = addr + size;
}

/* ---- The table ---------------------------------------------------------- */

void ops_fpu_init(op_table *tb)
{
    if (tb->fpu == RELEVO_FPU_NONE)
        return;
    for (uint16_t ea = 0; ea < 64; ea++) {
        table_set(tb, (uint16_t)(0xF200 | ea), op_fgen);
        table_set(tb, (uint16_t)(0xF240 | ea), op_fcc_misc);
        table_set(tb, (uint16_t)(0xF280 | ea), op_fbcc);
        table_set(tb, (uint16_t)(0xF2C0 | ea), op_fbcc);
        table_set(tb, (uint16_t)(0xF300 | ea), op_fsave);
        table_set(tb, (uint16_t)(0xF340 | ea), op_frestore);
    }
}
