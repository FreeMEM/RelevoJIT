/* SPDX-License-Identifier: MPL-2.0
 *
 * Extended precision arithmetic of the 68881 family (see fpx.h). Written
 * from the MC68881/MC68882 User's Manual and the IEEE 754 rules it
 * follows; the mantissas are computed exactly with 128-bit integers made
 * of two 64-bit halves (no compiler extension), then rounded once.
 */
#include "fpx.h"

#define BIAS 16383
#define EMIN (-16382)
#define EMAX 16383

/* ---- 128-bit helpers -------------------------------------------------- */

typedef struct u128 {
    uint64_t hi, lo;
} u128;

static u128 u128_of(uint64_t hi, uint64_t lo)
{
    u128 r = { hi, lo };
    return r;
}

static bool u128_zero(u128 a)
{
    return (a.hi | a.lo) == 0;
}

static int u128_cmp(u128 a, u128 b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

static u128 u128_add(u128 a, u128 b, unsigned *carry)
{
    u128 r;
    r.lo = a.lo + b.lo;
    uint64_t c = r.lo < a.lo;
    r.hi = a.hi + b.hi + c;
    *carry = (r.hi < a.hi) || (c && r.hi == a.hi);
    return r;
}

static u128 u128_sub(u128 a, u128 b)
{
    u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

static u128 u128_shl(u128 a, unsigned n)
{
    if (n == 0)
        return a;
    if (n >= 128)
        return u128_of(0, 0);
    if (n >= 64)
        return u128_of(a.lo << (n - 64), 0);
    return u128_of(a.hi << n | a.lo >> (64 - n), a.lo << n);
}

/* Shift right; the bits shifted out are ORed into bit 0 (sticky). */
static u128 u128_shr_sticky(u128 a, unsigned n)
{
    if (n == 0)
        return a;
    if (n >= 128)
        return u128_of(0, u128_zero(a) ? 0 : 1);
    uint64_t lost;
    u128 r;
    if (n >= 64) {
        lost = a.lo | (n > 64 ? a.hi << (128 - n) : 0);
        r = u128_of(0, a.hi >> (n - 64));
    } else {
        lost = a.lo << (64 - n);
        r = u128_of(a.hi >> n, a.lo >> n | a.hi << (64 - n));
    }
    r.lo |= lost != 0;
    return r;
}

static u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    u128 r;
    r.lo = (mid << 32) | (uint32_t)p00;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

unsigned fpx_clz64(uint64_t v)
{
#if defined(__GNUC__)
    return (unsigned)__builtin_clzll(v);
#else
    unsigned n = 0;
    while (!(v & 0x8000000000000000ull)) {
        v <<= 1;
        n++;
    }
    return n;
#endif
}

/* ---- Unpacked values -------------------------------------------------- */

enum { C_ZERO, C_NORM, C_INF, C_NAN };

typedef struct unp {
    int cls;
    bool sign;
    int32_t exp;        /* of bit 63 of m */
    uint64_t m;         /* C_NORM: bit 63 set */
} unp;

static unp unpack(fpx a)
{
    unp u;
    unsigned e = fpx_exp(a);
    u.sign = fpx_sign(a);
    u.m = a.mantissa;
    u.exp = 0;
    if (e == 0x7FFF) {
        u.cls = (a.mantissa << 1) ? C_NAN : C_INF;
        return u;
    }
    if (a.mantissa == 0) {
        u.cls = C_ZERO;
        return u;
    }
    u.cls = C_NORM;
    unsigned s = fpx_clz64(u.m);
    u.m <<= s;
    /* A denormal's exponent field 0 means the same scale as 1. */
    u.exp = (int32_t)(e == 0 ? 1 : e) - BIAS - (int32_t)s;
    return u;
}

static fpx zero(bool sign)
{
    return fpx_make(sign, 0, 0);
}

static fpx inf(bool sign)
{
    return fpx_make(sign, 0x7FFF, 0);
}

static fpx operr(fpx_env *e)
{
    e->flags |= FPX_OPERR;
    return fpx_default_nan();
}

static fpx quiet(fpx a)
{
    a.mantissa |= 0x4000000000000000ull;
    return a;
}

/* The NaN result of an operation with at least one NaN operand: the
 * destination `a` if it is one, else `b`. */
static fpx nan2(fpx_env *e, fpx a, fpx b)
{
    if (fpx_is_snan(a) || fpx_is_snan(b))
        e->flags |= FPX_SNAN;
    return quiet(fpx_is_nan(a) ? a : b);
}

static fpx nan1(fpx_env *e, fpx a)
{
    if (fpx_is_snan(a))
        e->flags |= FPX_SNAN;
    return quiet(a);
}

uint32_t fpx_cc(fpx a)
{
    uint32_t cc = fpx_sign(a) ? FPX_CC_N : 0;
    if (fpx_is_nan(a))
        cc |= FPX_CC_NAN;
    else if (fpx_is_inf(a))
        cc |= FPX_CC_I;
    else if (a.mantissa == 0)
        cc |= FPX_CC_Z;
    return cc;
}

unsigned fpx_accrued(unsigned f)
{
    unsigned a = 0;
    if (f & (FPX_BSUN | FPX_SNAN | FPX_OPERR))
        a |= 0x80;
    if (f & FPX_OVFL)
        a |= 0x40;
    if ((f & FPX_UNFL) && (f & FPX_INEX2))
        a |= 0x20;
    if (f & FPX_DZ)
        a |= 0x10;
    if (f & (FPX_INEX1 | FPX_INEX2 | FPX_OVFL))
        a |= 0x08;
    return a;
}

/* ---- Rounding --------------------------------------------------------- */

void fpx_round_raw(fpx_env *e, bool sign, int32_t *exp_io, uint64_t *hi_io,
                   uint64_t lo, unsigned prec, int32_t emin, int32_t emax)
{
    uint64_t hi = *hi_io;
    int32_t exp = *exp_io;
    if (hi == 0 && lo == 0) {
        *hi_io = 0;
        return;
    }
    /* Normalize: leading bit at 63 of hi. */
    if (hi == 0) {
        hi = lo;
        lo = 0;
        exp -= 64;
    }
    unsigned s = fpx_clz64(hi);
    if (s) {
        u128 v = u128_shl(u128_of(hi, lo), s);
        hi = v.hi;
        lo = v.lo;
        exp -= (int32_t)s;
    }
    bool tiny = exp < emin;
    if (tiny) {
        int64_t d = (int64_t)emin - exp;
        u128 v = u128_shr_sticky(u128_of(hi, lo), d > 200 ? 200u : (unsigned)d);
        hi = v.hi;
        lo = v.lo;
        exp = emin;
    }
    unsigned drop = 64 - prec;      /* bits of hi below the precision */
    bool rbit, sticky;
    if (drop == 0) {
        rbit = lo >> 63;
        sticky = (lo << 1) != 0;
    } else {
        rbit = (hi >> (drop - 1)) & 1;
        sticky = (hi & ((1ull << (drop - 1)) - 1)) != 0 || lo != 0;
    }
    uint64_t keep = drop ? ~((1ull << drop) - 1) : ~0ull;
    uint64_t r = hi & keep;
    bool inexact = rbit || sticky;
    bool inc;
    switch (e->mode) {
    case FPX_RN: inc = rbit && (sticky || ((r >> drop) & 1)); break;
    case FPX_RZ: inc = false; break;
    case FPX_RM: inc = inexact && sign; break;
    default:     inc = inexact && !sign; break;
    }
    if (inc) {
        uint64_t one = 1ull << drop;
        r += one;
        if (r < one) {                  /* carried out of bit 63 */
            r = 0x8000000000000000ull;
            exp++;
        }
    }
    if (tiny)
        e->flags |= FPX_UNFL;
    if (inexact)
        e->flags |= FPX_INEX2;
    if (exp > emax) {
        e->flags |= FPX_OVFL | FPX_INEX2;
        bool to_inf = e->mode == FPX_RN || (e->mode == FPX_RM && sign)
                   || (e->mode == FPX_RP && !sign);
        if (to_inf) {
            *exp_io = emax + 1;
            *hi_io = 0;
            return;
        }
        exp = emax;
        r = keep;
    }
    *exp_io = exp;
    *hi_io = r;
}

fpx fpx_pack(fpx_env *e, bool sign, int32_t exp, uint64_t hi, uint64_t lo)
{
    fpx_round_raw(e, sign, &exp, &hi, lo, e->prec, EMIN, EMAX);
    if (exp > EMAX)
        return inf(sign);
    if (hi == 0)
        return zero(sign);
    if (!(hi >> 63))
        return fpx_make(sign, 0, hi);   /* denormalized */
    return fpx_make(sign, (unsigned)(exp + BIAS), hi);
}

static fpx pack_unp(fpx_env *e, unp u)
{
    return fpx_pack(e, u.sign, u.exp, u.m, 0);
}

/* ---- Operations ------------------------------------------------------- */

static fpx add_unp(fpx_env *e, unp a, unp b)
{
    if (a.cls == C_ZERO && b.cls == C_ZERO) {
        bool s = a.sign == b.sign ? a.sign : e->mode == FPX_RM;
        return zero(s);
    }
    if (a.cls == C_ZERO)
        return pack_unp(e, b);
    if (b.cls == C_ZERO)
        return pack_unp(e, a);
    if (a.exp < b.exp || (a.exp == b.exp && a.m < b.m)) {
        unp t = a;
        a = b;
        b = t;
    }
    int64_t d = (int64_t)a.exp - b.exp;
    u128 x = u128_of(a.m, 0);
    u128 y = u128_shr_sticky(u128_of(b.m, 0), d > 200 ? 200u : (unsigned)d);
    int32_t exp = a.exp;
    if (a.sign == b.sign) {
        unsigned carry;
        u128 s = u128_add(x, y, &carry);
        if (carry) {
            s = u128_shr_sticky(s, 1);
            s.hi |= 0x8000000000000000ull;
            exp++;
        }
        return fpx_pack(e, a.sign, exp, s.hi, s.lo);
    }
    u128 s = u128_sub(x, y);
    if (u128_zero(s))
        return zero(e->mode == FPX_RM);
    return fpx_pack(e, a.sign, exp, s.hi, s.lo);
}

static fpx addsub(fpx_env *e, fpx a, fpx b, bool sub)
{
    if (fpx_is_nan(a) || fpx_is_nan(b))
        return nan2(e, a, b);
    unp ua = unpack(a), ub = unpack(b);
    ub.sign ^= sub;
    if (ua.cls == C_INF) {
        if (ub.cls == C_INF && ua.sign != ub.sign)
            return operr(e);
        return inf(ua.sign);
    }
    if (ub.cls == C_INF)
        return inf(ub.sign);
    return add_unp(e, ua, ub);
}

fpx fpx_add(fpx_env *e, fpx a, fpx b)
{
    return addsub(e, a, b, false);
}

fpx fpx_sub(fpx_env *e, fpx a, fpx b)
{
    return addsub(e, a, b, true);
}

fpx fpx_mul(fpx_env *e, fpx a, fpx b)
{
    if (fpx_is_nan(a) || fpx_is_nan(b))
        return nan2(e, a, b);
    unp ua = unpack(a), ub = unpack(b);
    bool s = ua.sign != ub.sign;
    if (ua.cls == C_INF || ub.cls == C_INF) {
        if (ua.cls == C_ZERO || ub.cls == C_ZERO)
            return operr(e);
        return inf(s);
    }
    if (ua.cls == C_ZERO || ub.cls == C_ZERO)
        return zero(s);
    u128 p = mul64(ua.m, ub.m);
    return fpx_pack(e, s, ua.exp + ub.exp + 1, p.hi, p.lo);
}

/* 128 bits of x / y (both with bit 63 set): floor(x / y * 2^127), and
 * whether there is a remainder. */
static u128 div128(uint64_t x, uint64_t y, bool *rest)
{
    uint64_t r = x;
    unsigned carry = 0;
    u128 q = u128_of(0, 0);
    for (int i = 0; i < 128; i++) {
        q = u128_shl(q, 1);
        if (carry || r >= y) {
            r -= y;
            q.lo |= 1;
        }
        carry = (unsigned)(r >> 63);
        r <<= 1;
    }
    *rest = r != 0 || carry;
    return q;
}

fpx fpx_div(fpx_env *e, fpx a, fpx b)
{
    if (fpx_is_nan(a) || fpx_is_nan(b))
        return nan2(e, a, b);
    unp ua = unpack(a), ub = unpack(b);
    bool s = ua.sign != ub.sign;
    if (ua.cls == C_INF)
        return ub.cls == C_INF ? operr(e) : inf(s);
    if (ub.cls == C_INF)
        return zero(s);
    if (ub.cls == C_ZERO) {
        if (ua.cls == C_ZERO)
            return operr(e);
        e->flags |= FPX_DZ;
        return inf(s);
    }
    if (ua.cls == C_ZERO)
        return zero(s);
    bool rest;
    u128 q = div128(ua.m, ub.m, &rest);
    q.lo |= rest;
    return fpx_pack(e, s, ua.exp - ub.exp, q.hi, q.lo);
}

fpx fpx_sqrt(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    unp u = unpack(a);
    if (u.cls == C_ZERO)
        return zero(u.sign);
    if (u.sign)
        return operr(e);
    if (u.cls == C_INF)
        return inf(false);
    /* value = m * 2^(exp - 63) = R * 2^k with k even */
    u128 rad;
    int32_t k;
    if (u.exp & 1) {
        rad = u128_of(u.m, 0);
        k = u.exp - 127;
    } else {
        rad = u128_of(u.m >> 1, u.m << 63);
        k = u.exp - 126;
    }
    /* Digit by digit: root = floor(sqrt(rad)), rem = rad - root^2. */
    u128 rem = u128_of(0, 0);
    uint64_t root = 0;
    for (int i = 63; i >= 0; i--) {
        uint64_t two = i >= 32 ? (rad.hi >> (2 * (i - 32))) & 3
                               : (rad.lo >> (2 * i)) & 3;
        rem = u128_shl(rem, 2);
        rem.lo |= two;
        u128 trial = u128_shl(u128_of(0, root), 2);
        trial.lo |= 1;
        root <<= 1;
        if (u128_cmp(rem, trial) >= 0) {
            rem = u128_sub(rem, trial);
            root |= 1;
        }
    }
    /* The next bit is 1 when rem > root; then there is more after it. */
    uint64_t lo;
    if (u128_cmp(rem, u128_of(0, root)) > 0)
        lo = 0x8000000000000001ull;
    else
        lo = u128_zero(rem) ? 0 : 1;
    return fpx_pack(e, false, k / 2 + 63, root, lo);
}

fpx fpx_round(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    unp u = unpack(a);
    if (u.cls == C_INF)
        return inf(u.sign);
    if (u.cls == C_ZERO)
        return zero(u.sign);
    return pack_unp(e, u);
}

fpx fpx_abs(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    a.sign_exponent &= 0x7FFF;
    return fpx_round(e, a);
}

fpx fpx_neg(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    a.sign_exponent ^= 0x8000;
    return fpx_round(e, a);
}

fpx fpx_int(fpx_env *e, fpx a, unsigned mode)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    unp u = unpack(a);
    if (u.cls == C_INF)
        return inf(u.sign);
    if (u.cls == C_ZERO)
        return zero(u.sign);
    if (u.exp >= 63)
        return pack_unp(e, u);
    /* integer part and the bits below it */
    uint64_t n;
    bool rbit, sticky;
    if (u.exp < 0) {
        n = 0;
        rbit = u.exp == -1;
        sticky = u.exp < -1 || (u.m << 1) != 0;
    } else {
        unsigned fb = 63 - (unsigned)u.exp;     /* fraction bits, 1..63 */
        n = u.m >> fb;
        rbit = (u.m >> (fb - 1)) & 1;
        sticky = (u.m & ((1ull << (fb - 1)) - 1)) != 0;
    }
    bool inexact = rbit || sticky, inc;
    switch (mode) {
    case FPX_RN: inc = rbit && (sticky || (n & 1)); break;
    case FPX_RZ: inc = false; break;
    case FPX_RM: inc = inexact && u.sign; break;
    default:     inc = inexact && !u.sign; break;
    }
    n += inc;
    if (inexact)
        e->flags |= FPX_INEX2;
    if (n == 0)
        return zero(u.sign);
    return fpx_pack(e, u.sign, 63, n, 0);
}

fpx fpx_getexp(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    unp u = unpack(a);
    if (u.cls == C_INF)
        return operr(e);
    if (u.cls == C_ZERO)
        return zero(u.sign);
    return fpx_from_int(e, u.exp);
}

fpx fpx_getman(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    unp u = unpack(a);
    if (u.cls == C_INF)
        return operr(e);
    if (u.cls == C_ZERO)
        return zero(u.sign);
    u.exp = 0;
    return pack_unp(e, u);
}

fpx fpx_scale(fpx_env *e, fpx a, fpx b)
{
    if (fpx_is_nan(a) || fpx_is_nan(b))
        return nan2(e, a, b);
    unp ua = unpack(a), ub = unpack(b);
    if (ub.cls == C_INF)
        return operr(e);
    if (ua.cls == C_INF)
        return inf(ua.sign);
    if (ua.cls == C_ZERO)
        return zero(ua.sign);
    /* the scale, truncated toward zero, limited to well past the range */
    int32_t n = 0;
    if (ub.cls == C_NORM && ub.exp >= 0)
        n = ub.exp > 16 ? 0x20000 : (int32_t)(ub.m >> (63 - ub.exp));
    if (ub.sign)
        n = -n;
    return fpx_pack(e, ua.sign, ua.exp + n, ua.m, 0);
}

fpx fpx_rem(fpx_env *e, fpx a, fpx b, bool ieee, uint8_t *quotient)
{
    unp ua = unpack(a), ub = unpack(b);
    uint8_t qsign = (ua.sign != ub.sign) ? 0x80 : 0;
    *quotient = qsign;
    if (fpx_is_nan(a) || fpx_is_nan(b)) {
        *quotient = 0;
        return nan2(e, a, b);
    }
    if (ua.cls == C_INF || ub.cls == C_ZERO) {
        *quotient = 0;
        return operr(e);
    }
    if (ua.cls == C_ZERO)
        return zero(ua.sign);
    if (ub.cls == C_INF)
        return fpx_round(e, a);
    /* r = |a| - q |b| with q = trunc(|a| / |b|), bit by bit */
    uint64_t r = ua.m, y = ub.m, q = 0;
    int64_t d = (int64_t)ua.exp - ub.exp;
    int32_t rexp = ua.exp;       /* exponent of bit 63 of r */
    if (d >= 0) {
        unsigned carry = 0;
        for (int64_t i = 0; i <= d; i++) {
            q <<= 1;
            if (carry || r >= y) {
                r -= y;
                q |= 1;
            }
            if (i < d) {
                carry = (unsigned)(r >> 63);
                r <<= 1;
            }
        }
        rexp = ub.exp;
    }
    bool rsign = ua.sign;
    if (ieee) {
        /* round the quotient to nearest even: compare 2r with y */
        int c;
        if (d < -1 || r == 0) {
            c = -1;
        } else if (d == -1) {
            /* |a| = r * 2^(rexp-63) with rexp = ub.exp - 1: 2r vs y */
            c = r < y ? -1 : r > y ? 1 : 0;
        } else {
            c = (r >> 63) ? 1 : (r << 1) < y ? -1 : (r << 1) > y ? 1 : 0;
        }
        if (c > 0 || (c == 0 && (q & 1))) {
            /* r' = y - r, in the scale of y */
            if (d == -1) {
                /* a has exponent ub.exp - 1: y - r/2 */
                u128 yy = u128_of(y, 0), rr = u128_shr_sticky(u128_of(r, 0), 1);
                u128 s = u128_sub(yy, rr);
                q++;
                *quotient = (uint8_t)(qsign | (q & 0x7F));
                return fpx_pack(e, !rsign, ub.exp, s.hi, s.lo);
            }
            r = y - r;
            q++;
            rsign = !rsign;
        }
    }
    *quotient = (uint8_t)(qsign | (q & 0x7F));
    if (r == 0)
        return zero(ua.sign);
    return fpx_pack(e, rsign, rexp, r, 0);
}

uint32_t fpx_cmp(fpx_env *e, fpx a, fpx b)
{
    if (fpx_is_nan(a) || fpx_is_nan(b)) {
        nan2(e, a, b);
        return FPX_CC_NAN;
    }
    unp ua = unpack(a), ub = unpack(b);
    if (ua.cls == C_INF && ub.cls == C_INF && ua.sign == ub.sign)
        return FPX_CC_Z | (ua.sign ? FPX_CC_N : 0);
    /* order: -inf < negatives < zeros < positives < +inf */
    int c;
    if (ua.cls == C_ZERO && ub.cls == C_ZERO) {
        c = 0;
    } else if (ua.cls == C_ZERO || ub.cls == C_ZERO) {
        c = ua.cls == C_ZERO ? (ub.sign ? 1 : -1) : (ua.sign ? -1 : 1);
    } else if (ua.sign != ub.sign) {
        c = ua.sign ? -1 : 1;
    } else {
        int mag;
        if (ua.cls == C_INF || ub.cls == C_INF)
            mag = ua.cls == C_INF ? 1 : -1;
        else if (ua.exp != ub.exp)
            mag = ua.exp > ub.exp ? 1 : -1;
        else
            mag = ua.m == ub.m ? 0 : ua.m > ub.m ? 1 : -1;
        c = ua.sign ? -mag : mag;
    }
    if (c < 0)
        return FPX_CC_N;
    if (c > 0)
        return 0;
    /* equal: the sign of the zero a - b would give */
    bool neg;
    if (ua.cls == C_ZERO)
        neg = (ua.sign && !ub.sign) || (ua.sign == ub.sign && e->mode == FPX_RM);
    else
        neg = e->mode == FPX_RM;
    return FPX_CC_Z | (neg ? FPX_CC_N : 0);
}

/* ---- Conversions ------------------------------------------------------ */

fpx fpx_from_int(fpx_env *e, int32_t v)
{
    if (v == 0)
        return zero(false);
    bool s = v < 0;
    uint64_t m = s ? 0u - (uint64_t)(int64_t)v : (uint64_t)v;
    return fpx_pack(e, s, 63, m, 0);
}

fpx fpx_from_single(uint32_t v)
{
    bool s = v >> 31;
    unsigned ex = (v >> 23) & 0xFF;
    uint64_t f = v & 0x7FFFFF;
    if (ex == 0xFF)
        return fpx_make(s, 0x7FFF, f ? 0x8000000000000000ull | f << 40 : 0);
    if (ex == 0) {
        if (f == 0)
            return zero(s);
        unsigned sh = fpx_clz64(f);
        /* value = f * 2^-149, leading bit at 63 - sh */
        return fpx_make(s, (unsigned)(63 - (int)sh - 149 + BIAS), f << sh);
    }
    return fpx_make(s, ex - 127 + BIAS, 0x8000000000000000ull | f << 40);
}

fpx fpx_from_double(uint64_t v)
{
    bool s = v >> 63;
    unsigned ex = (v >> 52) & 0x7FF;
    uint64_t f = v & 0xFFFFFFFFFFFFFull;
    if (ex == 0x7FF)
        return fpx_make(s, 0x7FFF, f ? 0x8000000000000000ull | f << 11 : 0);
    if (ex == 0) {
        if (f == 0)
            return zero(s);
        unsigned sh = fpx_clz64(f);
        return fpx_make(s, (unsigned)(63 - (int)sh - 1074 + BIAS), f << sh);
    }
    return fpx_make(s, ex - 1023 + BIAS, 0x8000000000000000ull | f << 11);
}

int32_t fpx_to_int(fpx_env *e, fpx a, unsigned size)
{
    int64_t max = (int64_t)1 << (8 * size - 1);    /* |min|; max is one less */
    if (fpx_is_nan(a)) {
        nan1(e, a);
        e->flags |= FPX_OPERR;
        return (int32_t)(fpx_sign(a) ? -max : max - 1);
    }
    unp u = unpack(a);
    if (u.cls == C_INF) {
        e->flags |= FPX_OPERR;
        return (int32_t)(u.sign ? -max : max - 1);
    }
    if (u.cls == C_ZERO)
        return 0;
    fpx_env t = *e;
    t.flags = 0;
    t.prec = 64;
    fpx r = fpx_int(&t, a, e->mode);
    unp ur = unpack(r);
    if (ur.cls == C_ZERO) {
        e->flags |= t.flags & FPX_INEX2;
        return 0;
    }
    uint64_t mag = ur.exp >= 63 ? ~0ull : ur.m >> (63 - ur.exp);
    if (ur.exp >= 40 || (int64_t)mag > (ur.sign ? max : max - 1)) {
        e->flags |= FPX_OPERR;
        return (int32_t)(ur.sign ? -max : max - 1);
    }
    e->flags |= t.flags & FPX_INEX2;
    return (int32_t)(ur.sign ? -(int64_t)mag : (int64_t)mag);
}

/* To single (fbits 23, emin -126, emax 127) or double (52, -1022, 1023). */
static uint64_t to_ieee(fpx_env *e, fpx a, unsigned fbits, int32_t emin,
                        int32_t emax)
{
    unsigned ebits = fbits == 23 ? 8 : 11;
    uint64_t emask = (1ull << ebits) - 1;
    uint64_t sbit = (uint64_t)fpx_sign(a) << (fbits + ebits);
    if (fpx_is_nan(a)) {
        fpx q = nan1(e, a);
        return sbit | emask << fbits | (q.mantissa << 1) >> (64 - fbits);
    }
    unp u = unpack(a);
    if (u.cls == C_INF)
        return sbit | emask << fbits;
    if (u.cls == C_ZERO)
        return sbit;
    int32_t exp = u.exp;
    uint64_t m = u.m;
    fpx_round_raw(e, u.sign, &exp, &m, 0, fbits + 1, emin, emax);
    if (exp > emax)
        return sbit | emask << fbits;
    if (m == 0)
        return sbit;
    uint64_t f = (m << 1) >> (64 - fbits);
    if (!(m >> 63))     /* denormalized */
        return sbit | m >> (63 - fbits);
    return sbit | (uint64_t)(exp - emin + 1) << fbits | f;
}

uint32_t fpx_to_single(fpx_env *e, fpx a)
{
    return (uint32_t)to_ieee(e, a, 23, -126, 127);
}

uint64_t fpx_to_double(fpx_env *e, fpx a)
{
    return to_ieee(e, a, 52, -1022, 1023);
}

fpx fpx_to_extended(fpx_env *e, fpx a)
{
    if (fpx_is_nan(a))
        return nan1(e, a);
    return a;
}
