/* SPDX-License-Identifier: MPL-2.0
 *
 * What the tests call besides include/relevo.h: test hooks the library
 * exports (src/hooks.rs and the end of src/capi.rs) and a few constants of
 * the 68k. Not part of the API: a program that uses the library needs only
 * relevo.h.
 */
#ifndef RELEVO_TEST_HOOKS_H
#define RELEVO_TEST_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

#include "relevo.h"

/* ---- The 68k --------------------------------------------------------- */

/* Exception vector numbers */
enum {
    VEC_RESET_SP = 0,
    VEC_RESET_PC = 1,
    VEC_BUS_ERROR = 2,
    VEC_ADDRESS_ERROR = 3,
    VEC_ILLEGAL = 4,
    VEC_ZERO_DIVIDE = 5,
    VEC_CHK = 6,
    VEC_TRAPV = 7,
    VEC_PRIVILEGE = 8,
    VEC_TRACE = 9,
    VEC_LINE_A = 10,
    VEC_LINE_F = 11,
    VEC_FORMAT_ERROR = 14,
    VEC_SPURIOUS = 24,
    VEC_AUTOVECTOR = 24,        /* + interrupt level 1-7 */
    VEC_TRAP = 32               /* + 0-15 */
};

/* ---- The interpreter ------------------------------------------------- */

/* Builds the opcode table of `model` with `fpu` and returns how many
 * opcodes two groups claimed (0 unless there is a bug). `table` is not
 * used (the table stays the library's): pass NULL. */
unsigned interp_fill_table(void *table, relevo_model model, relevo_fpu fpu);

/* A write to CACR, as MOVEC does it. */
void cache_write_cacr(relevo_cpu *cpu, uint32_t value);

/* The FPU's hidden state: a pending exception's vector (0: none) and
 * whether FSAVE gives a null frame. Reads it into *pending and *null
 * unless NULL, then sets it if `set` is not 0. */
void relevo_test_fpu_state(relevo_cpu *cpu, unsigned *pending, bool *null,
                           int set, unsigned set_pending, bool set_null);

/* ---- The JIT --------------------------------------------------------- */

typedef struct jit_stats {
    uint64_t blocks;        /* blocks translated */
    uint64_t insns;         /* instructions translated */
    uint64_t runs;          /* blocks entered */
    uint64_t bails;         /* left a block for the interpreter */
    uint64_t interp;        /* instructions run by the interpreter */
    uint64_t flushes;       /* whole cache flushes */
    uint64_t links;         /* jumps chained */
    uint64_t dropped;       /* blocks dropped (code changed, invalidate) */
    uint64_t host;          /* instructions a block ran in the interpreter
                             * for a host page */
} jit_stats;

/* Whether this build and this machine can run the JIT. */
bool jit_available(void);
void jit_get_stats(const relevo_cpu *cpu, jit_stats *out);
/* Uses MOVBE or not (when the CPU has it): to test both paths. Flushes. */
void jit_set_movbe(relevo_cpu *cpu, bool on);
/* Keeps condition codes in x86 form between blocks or not (when the CPU
 * has LAHF and SAHF): to test both paths. Flushes. */
void jit_set_lazy(relevo_cpu *cpu, bool on);
/* Whether the first instruction at `pc` would be translated. */
bool jit_translatable(relevo_cpu *cpu, uint32_t pc);

/* ---- Extended precision (fpx) ----------------------------------------
 *
 * The FPU's arithmetic (src/fpx), as the MC68881/MC68882 and the FPU of
 * the 68040 and 68060 compute it. A value is a relevo_fpreg: sign and
 * 15-bit biased exponent, then a 64-bit mantissa with an explicit integer
 * bit (MC68881UM 2.4). Each operation rounds to the precision and with the
 * rounding mode of its environment and ORs the exceptions it raises into
 * env->flags, with the bits of the FPSR exception byte (FPX_SNAN...).
 */

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

static inline bool fpx_is_nan(fpx a)
{
    return (a.sign_exponent & 0x7FFF) == 0x7FFF && (a.mantissa << 1) != 0;
}

static inline bool fpx_is_inf(fpx a)
{
    return (a.sign_exponent & 0x7FFF) == 0x7FFF && (a.mantissa << 1) == 0;
}

/* The condition code byte of the FPSR for a result. */
uint32_t fpx_cc(fpx a);
/* The accrued exception byte (FPSR bits 7-3) that the exception bits
 * `flags` add. */
unsigned fpx_accrued(unsigned flags);

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
/* FMOD (ieee false) and FREM (ieee true); *quotient gets the FPSR
 * quotient byte. */
fpx fpx_rem(fpx_env *e, fpx a, fpx b, bool ieee, uint8_t *quotient);
/* FCMP: the condition codes of a - b. */
uint32_t fpx_cmp(fpx_env *e, fpx a, fpx b);

fpx fpx_from_int(fpx_env *e, int32_t v);
fpx fpx_from_single(uint32_t v);
fpx fpx_from_double(uint64_t v);
/* To an integer of `size` bytes (1, 2, 4), rounded with the mode. */
int32_t fpx_to_int(fpx_env *e, fpx a, unsigned size);
uint32_t fpx_to_single(fpx_env *e, fpx a);
uint64_t fpx_to_double(fpx_env *e, fpx a);
/* FMOVE.X out: the register as it is, a signaling NaN made quiet. */
fpx fpx_to_extended(fpx_env *e, fpx a);

/* Packed decimal (MC68881UM 2.4.4): three longs. `k` is the k-factor
 * (-64..63). */
fpx fpx_from_packed(fpx_env *e, const uint32_t p[3]);
void fpx_to_packed(fpx_env *e, fpx a, int k, uint32_t p[3]);

/* FMOVECR: the constant at `offset` of the ROM, rounded with the
 * environment. Returns false for an offset with no constant. */
bool fpx_constant(fpx_env *e, unsigned offset, fpx *out);

/* The transcendental functions. */
typedef enum fpx_func {
    FPX_SINH, FPX_LOGNP1, FPX_ETOXM1, FPX_TANH, FPX_ATAN, FPX_ASIN,
    FPX_ATANH, FPX_SIN, FPX_TAN, FPX_ETOX, FPX_TWOTOX, FPX_TENTOX,
    FPX_LOGN, FPX_LOG10, FPX_LOG2, FPX_COSH, FPX_ACOS, FPX_COS
} fpx_func;

fpx fpx_trans(fpx_env *e, fpx_func f, fpx a);

#endif /* RELEVO_TEST_HOOKS_H */
