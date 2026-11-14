/* SPDX-License-Identifier: MPL-2.0
 *
 * Extended precision floating point, as the MC68881/MC68882 and the FPU of
 * the 68040 and 68060 compute it, in portable C. Shared by the engines.
 *
 * A value is a relevo_fpreg: sign and 15-bit biased exponent, then a 64-bit
 * mantissa with an explicit integer bit (MC68881UM 2.4). Inputs may be
 * denormalized (exponent 0) or unnormalized (integer bit 0, exponent not
 * 0); every result is normalized, or denormalized when it is too small.
 *
 * Each operation rounds to the precision and with the rounding mode of its
 * environment and ORs the exceptions it raises into env->flags, with the
 * bits of the FPSR exception byte (FPX_SNAN...). The precision only rounds
 * the mantissa (24 bits for single, 53 for double): the exponent keeps the
 * extended range, as in a floating-point data register. Conversions to
 * single and double for memory use the range of those formats.
 *
 * Underflow (FPX_UNFL) is raised when the exact result is smaller than the
 * smallest normalized number (tininess before rounding), also when the
 * rounded result is exact; FPX_INEX2 tells whether it was. The accrued
 * byte of the FPSR takes UNFL only together with INEX2 (fpx_accrued()).
 *
 * NaNs: an operation on a signaling NaN raises FPX_SNAN and gives it back
 * quiet (bit 62 set). With two NaN operands the destination (the first
 * operand) wins. An invalid operation (FPX_OPERR) gives the default NaN,
 * all ones.
 */
#ifndef RELEVO_FPX_H
#define RELEVO_FPX_H

#include <stdbool.h>
#include <stdint.h>

#include "relevo.h"

typedef relevo_fpreg fpx;

/* Rounding modes: the RND field of the FPCR. */
enum { FPX_RN = 0, FPX_RZ = 1, FPX_RM = 2, FPX_RP = 3 };

/* Exception bits: the FPSR exception status byte (and the FPCR enable
 * byte), bits 15 to 8. */
enum {
    FPX_BSUN = 0x8000,
    FPX_SNAN = 0x4000,
    FPX_OPERR = 0x2000,
    FPX_OVFL = 0x1000,
    FPX_UNFL = 0x0800,
    FPX_DZ = 0x0400,
    FPX_INEX2 = 0x0200,
    FPX_INEX1 = 0x0100
};

/* FPSR condition code bits (31-24) */
enum {
    FPX_CC_N = 0x08000000,
    FPX_CC_Z = 0x04000000,
    FPX_CC_I = 0x02000000,
    FPX_CC_NAN = 0x01000000
};

typedef struct fpx_env {
    unsigned mode;      /* FPX_RN... */
    unsigned prec;      /* mantissa bits: 64, 53 or 24 */
    unsigned flags;     /* raised exceptions, FPX_* */
} fpx_env;

/* ---- Classes --------------------------------------------------------- */

static inline bool fpx_sign(fpx a)
{
    return (a.sign_exponent & 0x8000) != 0;
}

static inline unsigned fpx_exp(fpx a)
{
    return a.sign_exponent & 0x7FFF;
}

static inline bool fpx_is_nan(fpx a)
{
    return fpx_exp(a) == 0x7FFF && (a.mantissa << 1) != 0;
}

static inline bool fpx_is_snan(fpx a)
{
    return fpx_is_nan(a) && !(a.mantissa & 0x4000000000000000ull);
}

static inline bool fpx_is_inf(fpx a)
{
    return fpx_exp(a) == 0x7FFF && (a.mantissa << 1) == 0;
}

static inline bool fpx_is_zero(fpx a)
{
    return fpx_exp(a) != 0x7FFF && a.mantissa == 0;
}

static inline fpx fpx_make(bool sign, unsigned exp, uint64_t mantissa)
{
    fpx r;
    r.sign_exponent = (uint16_t)((sign ? 0x8000u : 0) | (exp & 0x7FFF));
    r.mantissa = mantissa;
    return r;
}

/* All ones: what an invalid operation gives, and the reset value of the
 * data registers. */
static inline fpx fpx_default_nan(void)
{
    return fpx_make(false, 0x7FFF, ~0ull);
}

/* The condition code byte of the FPSR for a result (N is the sign, also of
 * zeros, infinities and NaNs). */
uint32_t fpx_cc(fpx a);

/* The accrued exception byte (FPSR bits 7-3) that the exception bits
 * `flags` add: IOP from SNAN/OPERR (and BSUN), OVFL, UNFL with INEX2, DZ,
 * INEX from INEX1/INEX2/OVFL. */
unsigned fpx_accrued(unsigned flags);

/* ---- Arithmetic ------------------------------------------------------ */

/* The result `a op b` has `a` as the destination operand. */
fpx fpx_add(fpx_env *e, fpx a, fpx b);
fpx fpx_sub(fpx_env *e, fpx a, fpx b);
fpx fpx_mul(fpx_env *e, fpx a, fpx b);
fpx fpx_div(fpx_env *e, fpx a, fpx b);
fpx fpx_sqrt(fpx_env *e, fpx a);
/* FMOVE into a register: normalizes and rounds to the precision. */
fpx fpx_round(fpx_env *e, fpx a);
fpx fpx_abs(fpx_env *e, fpx a);
fpx fpx_neg(fpx_env *e, fpx a);
/* FINT (with the environment's mode) and FINTRZ (`mode` FPX_RZ). */
fpx fpx_int(fpx_env *e, fpx a, unsigned mode);
fpx fpx_getexp(fpx_env *e, fpx a);
fpx fpx_getman(fpx_env *e, fpx a);
/* a * 2^trunc(b) */
fpx fpx_scale(fpx_env *e, fpx a, fpx b);
/* FMOD (ieee false: quotient truncated) and FREM (ieee true: quotient
 * rounded to nearest even). *quotient gets the FPSR quotient byte: the
 * sign of the quotient and its 7 low bits. */
fpx fpx_rem(fpx_env *e, fpx a, fpx b, bool ieee, uint8_t *quotient);
/* FCMP: the condition codes of a - b (N, Z, NAN; I never). */
uint32_t fpx_cmp(fpx_env *e, fpx a, fpx b);

/* ---- Conversions ----------------------------------------------------- */

/* Integers and single/double to extended: exact, then rounded to the
 * environment's precision (a signaling NaN stays signaling: the operation
 * that uses it raises FPX_SNAN). */
fpx fpx_from_int(fpx_env *e, int32_t v);
fpx fpx_from_single(uint32_t v);
fpx fpx_from_double(uint64_t v);
/* To an integer of `size` bytes (1, 2, 4), rounded with the mode. Out of
 * range, an infinity or a NaN: FPX_OPERR and the largest integer of the
 * operand's sign. */
int32_t fpx_to_int(fpx_env *e, fpx a, unsigned size);
uint32_t fpx_to_single(fpx_env *e, fpx a);
uint64_t fpx_to_double(fpx_env *e, fpx a);
/* FMOVE.X out: the register as it is, a signaling NaN made quiet. */
fpx fpx_to_extended(fpx_env *e, fpx a);

/* Packed decimal (MC68881UM 2.4.4): three longs, the first one with the
 * signs, the exponent and the integer digit. fpx_from_packed rounds with
 * the environment and raises FPX_INEX1 if inexact. fpx_to_packed writes
 * the value with the k-factor `k` (-64..63): k > 0 significant digits (17
 * at most; more is FPX_OPERR), k <= 0 digits after the decimal point. */
fpx fpx_from_packed(fpx_env *e, const uint32_t p[3]);
void fpx_to_packed(fpx_env *e, fpx a, int k, uint32_t p[3]);

/* FMOVECR: the constant at `offset` of the ROM, rounded with the
 * environment. Returns false for an offset with no constant. */
bool fpx_constant(fpx_env *e, unsigned offset, fpx *out);

/* ---- Transcendental functions (fpx_trans.c) -------------------------- */

typedef enum fpx_func {
    FPX_SINH, FPX_LOGNP1, FPX_ETOXM1, FPX_TANH, FPX_ATAN, FPX_ASIN,
    FPX_ATANH, FPX_SIN, FPX_TAN, FPX_ETOX, FPX_TWOTOX, FPX_TENTOX,
    FPX_LOGN, FPX_LOG10, FPX_LOG2, FPX_COSH, FPX_ACOS, FPX_COS
} fpx_func;

/* Computed with the host's long double functions (C99 <math.h>), then
 * rounded with the environment. Exact for the special values; otherwise
 * within about one unit in the last place of the host's long double (64
 * bits of mantissa on x86, as the 68881). */
fpx fpx_trans(fpx_env *e, fpx_func f, fpx a);

/* ---- Internal, shared by fpx*.c --------------------------------------- */

/* value = (hi * 2^64 + lo) * 2^(exp - 127): rounds to `prec` bits with
 * the exponent range [emin, emax] (of the leading bit), denormalizing
 * below emin. Gives back sign, exponent and a 64-bit mantissa (leading bit
 * at 63 unless denormalized at emin, or 0 for a zero). Overflow gives an
 * infinity (*exp = emax + 1) or the largest number, as the mode says. */
void fpx_round_raw(fpx_env *e, bool sign, int32_t *exp, uint64_t *hi,
                   uint64_t lo, unsigned prec, int32_t emin, int32_t emax);
/* The same, in the extended range, packed. */
fpx fpx_pack(fpx_env *e, bool sign, int32_t exp, uint64_t hi, uint64_t lo);

/* Leading zeros of a nonzero 64-bit value. */
unsigned fpx_clz64(uint64_t v);

#endif
