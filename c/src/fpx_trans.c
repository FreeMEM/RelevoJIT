/* SPDX-License-Identifier: MPL-2.0
 *
 * The transcendental functions of the 68881 (see fpx.h): the special
 * operands (zeros, infinities, NaNs, out of the domain) as the MC68881UM
 * gives them, the rest through the host's C99 long double functions.
 *
 * Accuracy: what the host's long double gives. On x86-64 with glibc it is
 * the 64-bit mantissa of the 68881, usually within one unit in the last
 * place (the 68881 promises about the same); on hosts where long double is
 * a double, about 53 bits. The library rounds to nearest; the FPCR's mode
 * only acts when the result is rounded again to single or double
 * precision. INEX2 is set unless the result is exact (the special values,
 * and integer powers of 2 and 10 in FTWOTOX, FTENTOX, FLOG2 and FLOG10).
 */
#include <float.h>
#include <math.h>

#include "fpx.h"

#define BIAS 16383

static long double to_ld(fpx a)
{
    unsigned e = fpx_exp(a);
    long double m = (long double)a.mantissa;
    long double v = ldexpl(m, (int)(e == 0 ? 1 : e) - BIAS - 63);
    return fpx_sign(a) ? -v : v;
}

/* Rounds a long double result with the environment. */
static fpx from_ld(fpx_env *e, long double v, bool exact)
{
    fpx_env t = *e;
    t.flags = 0;
    bool s = signbit(v) != 0;
    fpx r;
    if (isnan(v)) {
        t.flags |= FPX_OPERR;
        r = fpx_default_nan();
    } else if (isinf(v)) {
        r = fpx_make(s, 0x7FFF, 0);
    } else if (v == 0) {
        r = fpx_make(s, 0, 0);
    } else {
        int ex;
        long double f = frexpl(fabsl(v), &ex);      /* [0.5, 1) */
        long double hi_ld = ldexpl(f, 64);
        uint64_t hi = (uint64_t)hi_ld;
        long double rest = hi_ld - (long double)hi;  /* when long double is wider */
        uint64_t lo = (uint64_t)ldexpl(rest, 64);
        r = fpx_pack(&t, s, ex - 1, hi, lo);
    }
    if (!exact)
        t.flags |= FPX_INEX2;
    e->flags |= t.flags;
    return r;
}

static fpx overflow(fpx_env *e, bool sign)
{
    /* far beyond the range: the mode's overflow result */
    return fpx_pack(e, sign, 0x10000, 0x8000000000000000ull, 0);
}

static fpx underflow(fpx_env *e, bool sign)
{
    return fpx_pack(e, sign, -0x10000, 0x8000000000000000ull, 0);
}

static fpx dz(fpx_env *e, bool sign)
{
    e->flags |= FPX_DZ;
    return fpx_make(sign, 0x7FFF, 0);
}

static fpx operr(fpx_env *e)
{
    e->flags |= FPX_OPERR;
    return fpx_default_nan();
}

static fpx one(bool sign)
{
    return fpx_make(sign, BIAS, 0x8000000000000000ull);
}

static fpx zero(bool sign)
{
    return fpx_make(sign, 0, 0);
}

/* Whether a finite x is an integer, and which (when it fits). */
static bool integral(long double x, long *n)
{
    if (x != floorl(x) || fabsl(x) > 100000)
        return false;
    *n = (long)x;
    return true;
}

/* An exp-like result: overflow and underflow when the long double could
 * not hold it. */
static fpx exp_result(fpx_env *e, long double r, bool exact)
{
    if (isinf(r))
        return overflow(e, false);
    if (r == 0)
        return underflow(e, false);
    return from_ld(e, r, exact);
}

fpx fpx_trans(fpx_env *e, fpx_func f, fpx a)
{
    if (fpx_is_nan(a)) {
        if (fpx_is_snan(a))
            e->flags |= FPX_SNAN;
        a.mantissa |= 0x4000000000000000ull;
        return a;
    }
    bool s = fpx_sign(a);
    bool isz = fpx_is_zero(a), isi = fpx_is_inf(a);
    long double x = isi ? (s ? -INFINITY : INFINITY) : to_ld(a);
    long n;

    switch (f) {
    case FPX_SIN:
    case FPX_TAN:
        if (isi)
            return operr(e);
        if (isz)
            return zero(s);
        return from_ld(e, f == FPX_SIN ? sinl(x) : tanl(x), false);
    case FPX_COS:
        if (isi)
            return operr(e);
        if (isz)
            return one(false);
        return from_ld(e, cosl(x), false);
    case FPX_ATAN:
        if (isz)
            return zero(s);
        return from_ld(e, atanl(x), false);
    case FPX_ASIN:
    case FPX_ACOS:
        if (isi || fabsl(x) > 1)
            return operr(e);
        if (f == FPX_ASIN && isz)
            return zero(s);
        if (f == FPX_ACOS && x == 1)
            return zero(false);
        return from_ld(e, f == FPX_ASIN ? asinl(x) : acosl(x), false);
    case FPX_ATANH:
        if (isi || fabsl(x) > 1)
            return operr(e);
        if (isz)
            return zero(s);
        if (fabsl(x) == 1)
            return dz(e, s);
        return from_ld(e, atanhl(x), false);
    case FPX_SINH:
        if (isi || isz)
            return isi ? fpx_make(s, 0x7FFF, 0) : zero(s);
        {
            long double r = sinhl(x);
            if (isinf(r))
                return overflow(e, s);
            return from_ld(e, r, false);
        }
    case FPX_COSH:
        if (isi)
            return fpx_make(false, 0x7FFF, 0);
        if (isz)
            return one(false);
        {
            long double r = coshl(x);
            if (isinf(r))
                return overflow(e, false);
            return from_ld(e, r, false);
        }
    case FPX_TANH:
        if (isi)
            return one(s);
        if (isz)
            return zero(s);
        return from_ld(e, tanhl(x), false);
    case FPX_ETOX:
    case FPX_TWOTOX:
    case FPX_TENTOX:
        if (isi)
            return s ? zero(false) : fpx_make(false, 0x7FFF, 0);
        if (isz)
            return one(false);
        if (f == FPX_ETOX)
            return exp_result(e, expl(x), false);
        if (f == FPX_TWOTOX)
            return exp_result(e, exp2l(x), integral(x, &n));
        return exp_result(e, powl(10.0L, x), integral(x, &n) && n >= 0 && n <= 27);
    case FPX_ETOXM1:
        if (isi)
            return s ? one(true) : fpx_make(false, 0x7FFF, 0);
        if (isz)
            return zero(s);
        {
            long double r = expm1l(x);
            if (isinf(r))
                return overflow(e, false);
            return from_ld(e, r, false);
        }
    case FPX_LOGN:
    case FPX_LOG10:
    case FPX_LOG2:
        if (isz)
            return dz(e, true);
        if (s)
            return operr(e);
        if (isi)
            return fpx_make(false, 0x7FFF, 0);
        if (x == 1)
            return zero(false);
        if (f == FPX_LOGN)
            return from_ld(e, logl(x), false);
        if (f == FPX_LOG2) {
            long double r = log2l(x);
            return from_ld(e, r, a.mantissa << 1 == 0 || (fpx_exp(a) == 0
                                  && (a.mantissa & (a.mantissa - 1)) == 0));
        } else {
            long double r = log10l(x);
            bool exact = integral(r, &n) && n >= 0 && n <= 27
                      && powl(10.0L, (long double)n) == x;
            return from_ld(e, exact ? (long double)n : r, exact);
        }
    case FPX_LOGNP1:
        if (isi)
            return s ? operr(e) : fpx_make(false, 0x7FFF, 0);
        if (isz)
            return zero(s);
        if (x == -1)
            return dz(e, true);
        if (x < -1)
            return operr(e);
        return from_ld(e, log1pl(x), false);
    default:
        return operr(e);
    }
}
