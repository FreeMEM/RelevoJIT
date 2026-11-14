/* SPDX-License-Identifier: MPL-2.0
 *
 * Packed decimal conversions and the constant ROM of the 68881 family
 * (see fpx.h). The decimal conversions are exact: they work on big
 * integers, so the result is the value correctly rounded with the FPCR's
 * mode (the real chips use a fixed internal precision and may differ in
 * the last digit or bit for some values).
 */
#include <math.h>

#include "fpx.h"

#define BIAS 16383

/* ---- Big unsigned integers -------------------------------------------- */

/* 10^4966 times a 64-bit mantissa is the largest value needed: about
 * 16 560 bits. They live on the stack (2 KB each): no shared state. */
#define BIG_LIMBS 560

typedef struct big {
    unsigned n;                 /* limbs used; d[n - 1] != 0 unless n == 0 */
    uint32_t d[BIG_LIMBS];
} big;

static void big_trim(big *a)
{
    while (a->n > 0 && a->d[a->n - 1] == 0)
        a->n--;
}

static void big_set64(big *a, uint64_t v)
{
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> 32);
    a->n = 2;
    big_trim(a);
}

static void big_mul32(big *a, uint32_t m)
{
    uint64_t carry = 0;
    for (unsigned i = 0; i < a->n; i++) {
        uint64_t p = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)p;
        carry = p >> 32;
    }
    if (carry && a->n < BIG_LIMBS)
        a->d[a->n++] = (uint32_t)carry;
}

static void big_mul_pow10(big *a, unsigned n)
{
    for (; n >= 9; n -= 9)
        big_mul32(a, 1000000000u);
    static const uint32_t p10[9] = {
        1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
    };
    if (n)
        big_mul32(a, p10[n]);
}

static void big_shl(big *a, unsigned bits)
{
    if (a->n == 0)
        return;
    unsigned w = bits / 32, b = bits % 32;
    unsigned n = a->n + w + 1;
    if (n > BIG_LIMBS)
        n = BIG_LIMBS;
    for (int i = (int)n - 1; i >= 0; i--) {
        int src = i - (int)w;
        uint32_t hi = src >= 0 && src < (int)a->n ? a->d[src] : 0;
        uint32_t lo = src - 1 >= 0 && src - 1 < (int)a->n ? a->d[src - 1] : 0;
        a->d[i] = b ? (hi << b) | (lo >> (32 - b)) : hi;
    }
    a->n = n;
    big_trim(a);
}

static void big_shr1(big *a)
{
    for (unsigned i = 0; i < a->n; i++) {
        uint32_t next = i + 1 < a->n ? a->d[i + 1] : 0;
        a->d[i] = (a->d[i] >> 1) | (next << 31);
    }
    big_trim(a);
}

static unsigned big_bits(const big *a)
{
    if (a->n == 0)
        return 0;
    return 32 * a->n - (fpx_clz64(a->d[a->n - 1]) - 32);
}

static int big_cmp(const big *a, const big *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = (int)a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* a -= b, with a >= b */
static void big_sub(big *a, const big *b)
{
    uint64_t borrow = 0;
    for (unsigned i = 0; i < a->n; i++) {
        uint64_t s = (uint64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        a->d[i] = (uint32_t)s;
        borrow = (s >> 63) & 1;
    }
    big_trim(a);
}

static bool big_bit(const big *a, unsigned i)
{
    return i / 32 < a->n && ((a->d[i / 32] >> (i % 32)) & 1);
}

/* The quotient floor(a / b), known to be below 2^nbits (nbits <= 128),
 * into *hi:*lo; a is left with the remainder. */
static void big_div(big *a, const big *b, unsigned nbits, uint64_t *hi,
                    uint64_t *lo)
{
    big tt = *b;
    big_shl(&tt, nbits - 1);
    *hi = *lo = 0;
    for (int i = (int)nbits - 1; i >= 0; i--) {
        if (big_cmp(a, &tt) >= 0) {
            big_sub(a, &tt);
            if (i >= 64)
                *hi |= 1ull << (i - 64);
            else
                *lo |= 1ull << i;
        }
        big_shr1(&tt);
    }
}

/* The top 128 bits of a (with `bits` bits), the rest as a sticky bit. */
static void big_top128(const big *a, unsigned bits, uint64_t *hi,
                       uint64_t *lo)
{
    *hi = *lo = 0;
    bool sticky = false;
    for (unsigned k = 0; k < bits; k++) {
        unsigned i = bits - 1 - k;      /* bit of a, from the top */
        if (!big_bit(a, i))
            continue;
        if (k < 64)
            *hi |= 1ull << (63 - k);
        else if (k < 128)
            *lo |= 1ull << (127 - k);
        else
            sticky = true;
    }
    *lo |= sticky;
}

/* ---- Packed decimal ---------------------------------------------------- */

fpx fpx_from_packed(fpx_env *e, const uint32_t p[3])
{
    bool sign = p[0] >> 31;
    if ((p[0] & 0x7FFF0000) == 0x7FFF0000) {
        uint64_t m = (uint64_t)p[1] << 32 | p[2];
        return fpx_make(sign, 0x7FFF, m);
    }
    int exp10 = (int)(((p[0] >> 24) & 0xF) * 100 + ((p[0] >> 20) & 0xF) * 10
                      + ((p[0] >> 16) & 0xF));
    if (p[0] & 0x40000000)
        exp10 = -exp10;
    uint64_t digits = p[0] & 0xF;
    for (int i = 0; i < 16; i++) {
        uint32_t w = i < 8 ? p[1] : p[2];
        digits = digits * 10 + ((w >> (28 - 4 * (i % 8))) & 0xF);
    }
    if (digits == 0)
        return fpx_make(sign, 0, 0);

    big a, b;
    uint64_t hi, lo;
    int32_t exp;
    int n = exp10 - 16;
    big_set64(&a, digits);
    if (n >= 0) {
        big_mul_pow10(&a, (unsigned)n);
        unsigned bits = big_bits(&a);
        big_top128(&a, bits, &hi, &lo);
        exp = (int32_t)bits - 1;
    } else {
        big_set64(&b, 1);
        big_mul_pow10(&b, (unsigned)-n);
        unsigned s = 127 + big_bits(&b) - big_bits(&a);
        big_shl(&a, s);
        big_div(&a, &b, 128, &hi, &lo);
        lo |= a.n != 0;
        exp = 127 - (int32_t)s;
    }
    fpx_env t = *e;
    t.flags = 0;
    fpx r = fpx_pack(&t, sign, exp, hi, lo);
    e->flags |= (t.flags & ~(unsigned)FPX_INEX2)
              | ((t.flags & FPX_INEX2) ? FPX_INEX1 : 0);
    return r;
}

/* a = m * 2^p2 * 10^max(-t, 0), b = 10^max(t, 0) * 2^max(-p2, 0): a / b is
 * the value divided by 10^t. */
static void scaled(big *a, big *b, uint64_t m, int p2, int t)
{
    big_set64(a, m);
    big_set64(b, 1);
    if (p2 > 0)
        big_shl(a, (unsigned)p2);
    else if (p2 < 0)
        big_shl(b, (unsigned)-p2);
    if (t < 0)
        big_mul_pow10(a, (unsigned)-t);
    else
        big_mul_pow10(b, (unsigned)t);
}

static bool ge_pow10(uint64_t m, int p2, int t)
{
    big a, b;
    scaled(&a, &b, m, p2, t);
    return big_cmp(&a, &b) >= 0;
}

static uint32_t bcd3(unsigned v)
{
    return (v / 100 % 10) << 8 | (v / 10 % 10) << 4 | v % 10;
}

void fpx_to_packed(fpx_env *e, fpx x, int k, uint32_t p[3])
{
    bool sign = fpx_sign(x);
    p[0] = sign ? 0x80000000u : 0;
    p[1] = p[2] = 0;
    if (fpx_exp(x) == 0x7FFF) {
        if (fpx_is_nan(x)) {
            if (fpx_is_snan(x)) {
                e->flags |= FPX_SNAN;
                x.mantissa |= 0x4000000000000000ull;
            }
            p[1] = (uint32_t)(x.mantissa >> 32);
            p[2] = (uint32_t)x.mantissa;
        }
        p[0] |= 0x7FFF0000;
        return;
    }
    if (k > 17) {
        e->flags |= FPX_OPERR;
        k = 17;
    }
    if (x.mantissa == 0)
        return;
    /* value = m * 2^p2, m normalized */
    unsigned s = fpx_clz64(x.mantissa);
    uint64_t m = x.mantissa << s;
    int e2 = (int)(fpx_exp(x) == 0 ? 1 : fpx_exp(x)) - BIAS - (int)s;
    int p2 = e2 - 63;
    int e10 = (int)floor(e2 * 0.30102999566398119521);
    while (!ge_pow10(m, p2, e10))
        e10--;
    while (ge_pow10(m, p2, e10 + 1))
        e10++;
    int len = k > 0 ? k : e10 + 1 - k;
    if (len > 17)
        len = 17;
    if (len < 1)
        len = 1;

    big a, b, h;
    int t = e10 - len + 1;
    scaled(&a, &b, m, p2, t);
    uint64_t qhi, q;
    big_div(&a, &b, 60, &qhi, &q);
    bool rest = a.n != 0, inc;
    h = a;
    big_shl(&h, 1);
    int half = big_cmp(&h, &b);         /* 2 * remainder against b */
    switch (e->mode) {
    case FPX_RN: inc = half > 0 || (half == 0 && (q & 1)); break;
    case FPX_RZ: inc = false; break;
    case FPX_RM: inc = rest && sign; break;
    default:     inc = rest && !sign; break;
    }
    q += inc;
    if (rest)
        e->flags |= FPX_INEX2;
    uint64_t limit = 1;
    for (int i = 0; i < len; i++)
        limit *= 10;
    if (q >= limit) {               /* 9.99 became 10.0 */
        e10++;
        if (k > 0 || len == 17)
            q /= 10;
        else
            len++;
    }
    for (int i = len; i < 17; i++)
        q *= 10;
    /* 17 digits: the integer digit, then 16 */
    uint32_t dg[17];
    for (int i = 16; i >= 0; i--) {
        dg[i] = (uint32_t)(q % 10);
        q /= 10;
    }
    unsigned ae = (unsigned)(e10 < 0 ? -e10 : e10);
    if (ae > 999)
        e->flags |= FPX_OPERR;
    p[0] |= (e10 < 0 ? 0x40000000u : 0) | bcd3(ae % 1000) << 16
          | (uint32_t)(ae / 1000 % 10) << 12 | dg[0];
    for (int i = 0; i < 8; i++) {
        p[1] |= dg[1 + i] << (28 - 4 * i);
        p[2] |= dg[9 + i] << (28 - 4 * i);
    }
}

/* ---- The constant ROM ------------------------------------------------ */

/* Offset, exponent of the leading bit and 128 bits of mantissa (the last
 * bit set when the value goes on). Computed with exact big integer series.
 * As the real ROM, e and log10(2) round to nearest downwards: their low
 * half says "just above" the 64-bit value, so RN and RZ give it and RP the
 * next one. */
static const struct {
    uint8_t offset;
    int16_t exp;
    uint64_t hi, lo;
} rom[] = {
    { 0x00, 1, 0xC90FDAA22168C234ull, 0xC4C6628B80DC1CD1ull },  /* pi */
    { 0x0B, -2, 0x9A209A84FBCFF798ull, 0x0000000000000001ull }, /* log10(2) */
    { 0x0C, 1, 0xADF85458A2BB4A9Aull, 0x0000000000000001ull },  /* e */
    { 0x0D, 0, 0xB8AA3B295C17F0BBull, 0xBE87FED0691D3E89ull },  /* log2(e) */
    { 0x0E, -2, 0xDE5BD8A937287195ull, 0x355BAAAFAD33DC33ull }, /* log10(e) */
    { 0x0F, 0, 0, 0 },                                          /* 0.0 */
    { 0x30, -1, 0xB17217F7D1CF79ABull, 0xC9E3B39803F2F6AFull }, /* ln(2) */
    { 0x31, 1, 0x935D8DDDAAA8AC16ull, 0xEA56D62B82D30A29ull },  /* ln(10) */
    { 0x32, 0, 0x8000000000000000ull, 0 },                      /* 10^0 */
    { 0x33, 3, 0xA000000000000000ull, 0 },
    { 0x34, 6, 0xC800000000000000ull, 0 },
    { 0x35, 13, 0x9C40000000000000ull, 0 },
    { 0x36, 26, 0xBEBC200000000000ull, 0 },
    { 0x37, 53, 0x8E1BC9BF04000000ull, 0 },
    { 0x38, 106, 0x9DC5ADA82B70B59Dull, 0xF020000000000000ull },
    { 0x39, 212, 0xC2781F49FFCFA6D5ull, 0x3CBF6B71C76B25FBull },
    { 0x3A, 425, 0x93BA47C980E98CDFull, 0xC66F336C36B10137ull },
    { 0x3B, 850, 0xAA7EEBFB9DF9DE8Dull, 0xDDBB901B98FEEAB7ull },
    { 0x3C, 1700, 0xE319A0AEA60E91C6ull, 0xCC655C54BC5058F9ull },
    { 0x3D, 3401, 0xC976758681750C17ull, 0x650D3D28F18B50CFull },
    { 0x3E, 6803, 0x9E8B3B5DC53D5DE4ull, 0xA74D28CE329ACE53ull },
    { 0x3F, 13606, 0xC46052028A20979Aull, 0xC94C153F804A4A93ull }, /* 10^4096 */
};

bool fpx_constant(fpx_env *e, unsigned offset, fpx *out)
{
    for (size_t i = 0; i < sizeof rom / sizeof *rom; i++) {
        if (rom[i].offset != offset)
            continue;
        if (rom[i].hi == 0) {
            *out = fpx_make(false, 0, 0);
            return true;
        }
        /* The ROM's extended values count as exact: only a rounding to
         * single or double precision is inexact. */
        fpx_env t = *e;
        t.flags = 0;
        *out = fpx_pack(&t, false, rom[i].exp, rom[i].hi, rom[i].lo);
        if (e->prec < 64)
            e->flags |= t.flags;
        return true;
    }
    return false;
}
