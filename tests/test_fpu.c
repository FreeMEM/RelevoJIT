/* SPDX-License-Identifier: MPL-2.0
 *
 * Tests of the FPU (task R4): the extended arithmetic of src/fpx*.c and
 * the instructions of src/interp/ops_fpu.c. The expected values come from
 * the MC68881/MC68882 User's Manual, M68000PRM chapter 5 and MC68040UM
 * chapter 9, and from exact arithmetic; the opcodes were checked with
 * vasm.
 *
 * On x86-64 the basic operations are also compared with the x87 unit,
 * which computes in the same extended format with the same rounding
 * precision control (mantissa only, extended exponent range): random
 * operands, every rounding mode and precision, values and exceptions.
 */
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cpu.h"
#include "../src/fpx.h"
#include "check.h"
#include "tests.h"

static fpx X(unsigned se, uint64_t m)
{
    fpx r;
    r.sign_exponent = (uint16_t)se;
    r.mantissa = m;
    return r;
}

static fpx_env env(unsigned mode, unsigned prec)
{
    fpx_env e = { mode, prec, 0 };
    return e;
}

#define CHECK_X(v, se, m)                                                \
    do {                                                                 \
        fpx check_v_ = (v);                                              \
        CHECK_EQ(check_v_.sign_exponent, (se));                          \
        CHECK_EQ(check_v_.mantissa, (m));                                \
    } while (0)

static const fpx ONE = { 0x8000000000000000ull, 0x3FFF };
static const fpx TWO = { 0x8000000000000000ull, 0x4000 };
static const fpx THREE = { 0xC000000000000000ull, 0x4000 };

/* ---- Differential test against the x87 ----------------------------------- */

#if defined(__x86_64__) && defined(__GNUC__) && LDBL_MANT_DIG == 64
#include <fenv.h>

static void x87_control(unsigned mode, unsigned prec)
{
    static const unsigned rc[4] = { 0, 3, 1, 2 };   /* RN RZ RM RP */
    unsigned pc = prec == 24 ? 0 : prec == 53 ? 2 : 3;
    uint16_t cw = (uint16_t)(0x037F & ~0x0F00u);
    cw = (uint16_t)(cw | rc[mode] << 10 | pc << 8);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

static long double to_ld(fpx a)
{
    long double v = 0;
    unsigned char b[16] = {0};
    memcpy(b, &a.mantissa, 8);
    memcpy(b + 8, &a.sign_exponent, 2);
    memcpy(&v, b, 10);
    return v;
}

static fpx from_ld(long double v)
{
    fpx r;
    unsigned char b[16];
    memcpy(b, &v, 10);
    memcpy(&r.mantissa, b, 8);
    memcpy(&r.sign_exponent, b + 8, 2);
    return r;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* A random normalized or denormalized operand, often near the edges. */
static fpx random_operand(void)
{
    uint64_t m = rnd() | 0x8000000000000000ull;
    switch (rnd() % 8) {
    case 0: m &= 0xFFFFFF0000000000ull; break;      /* short mantissas */
    case 1: m |= 0x00000000FFFFFFFFull; break;
    default: break;
    }
    unsigned e;
    switch (rnd() % 6) {
    case 0: e = (unsigned)(rnd() % 0x7FFE) + 1; break;
    case 1: e = (unsigned)(rnd() % 80) + 1; break;               /* tiny */
    case 2: e = 0x7FFE - (unsigned)(rnd() % 80); break;          /* huge */
    case 3: e = 0; m >>= rnd() % 64; break;                      /* denormal */
    default: e = 0x3FFF + (unsigned)(rnd() % 130) - 65; break;
    }
    if (m == 0)
        m = 1;
    if (e == 0)
        m &= 0x7FFFFFFFFFFFFFFFull;
    return X((rnd() & 1) << 15 | e, m);
}

static unsigned x87_flags(void)
{
    unsigned f = 0;
    if (fetestexcept(FE_INEXACT))
        f |= FPX_INEX2;
    if (fetestexcept(FE_OVERFLOW))
        f |= FPX_OVFL;
    if (fetestexcept(FE_UNDERFLOW))
        f |= FPX_UNFL;
    if (fetestexcept(FE_DIVBYZERO))
        f |= FPX_DZ;
    if (fetestexcept(FE_INVALID))
        f |= FPX_OPERR;
    return f;
}

void test_fpx_against_x87(void)
{
    static const unsigned precs[3] = { 64, 53, 24 };
    unsigned bad = 0, shown = 0;
    for (unsigned n = 0; n < 400000; n++) {
        unsigned op = n % 5, mode = (n / 5) % 4, prec = precs[(n / 20) % 3];
        fpx a = random_operand(), b = random_operand();
        if (op == 4 && (n & 1))
            a.sign_exponent &= 0x7FFF;
        if ((n % 7) == 0 && op < 2 && (a.sign_exponent & 0x7FFF) != 0
            && (b.mantissa >> 63))      /* close exponents: cancellation */
            b.sign_exponent = (uint16_t)((b.sign_exponent & 0x8000)
                                         | (a.sign_exponent & 0x7FFF));
        fpx_env e = env(mode, prec);
        fpx r;
        switch (op) {
        case 0: r = fpx_add(&e, a, b); break;
        case 1: r = fpx_sub(&e, a, b); break;
        case 2: r = fpx_mul(&e, a, b); break;
        case 3: r = fpx_div(&e, a, b); break;
        default: r = fpx_sqrt(&e, a); break;
        }
        volatile long double x = to_ld(a), y = to_ld(b), z;
        feclearexcept(FE_ALL_EXCEPT);
        x87_control(mode, prec);
        switch (op) {
        case 0: z = x + y; break;
        case 1: z = x - y; break;
        case 2: z = x * y; break;
        case 3: z = x / y; break;
        default: __asm__ __volatile__("fsqrt" : "=t"(z) : "0"(x)); break;
        }
        x87_control(FPX_RN, 64);
        unsigned xf = x87_flags();
        fpx w = from_ld(z);
        if (fpx_is_inf(w))
            w.mantissa = 0;     /* the x87 sets the integer bit */
        /* The x87 signals underflow only when tiny and inexact (as the
         * 68881's accrued UNFL); a NaN is its own default NaN. */
        unsigned mine = e.flags & ~(unsigned)FPX_UNFL;
        if ((e.flags & FPX_UNFL) && (e.flags & FPX_INEX2))
            mine |= FPX_UNFL;
        bool same = (fpx_is_nan(r) && fpx_is_nan(w))
                 || (r.sign_exponent == w.sign_exponent
                     && r.mantissa == w.mantissa);
        if (!same || mine != xf) {
            bad++;
            if (shown++ < 10)
                printf("    x87: op %u mode %u prec %u: %04x:%016llx, "
                       "%04x:%016llx -> %04x:%016llx %04x / x87 "
                       "%04x:%016llx %04x\n", op, mode, prec,
                       a.sign_exponent, (unsigned long long)a.mantissa,
                       b.sign_exponent, (unsigned long long)b.mantissa,
                       r.sign_exponent, (unsigned long long)r.mantissa,
                       mine, w.sign_exponent,
                       (unsigned long long)w.mantissa, xf);
        }
    }
    CHECK_EQ(bad, 0);
}

/* FINT (frndint), FREM and FMOD (fprem1, fprem: the quotient's low 3
 * bits), FSCALE (fscale) and the conversions to single and double. */
void test_fpx_against_x87_more(void)
{
    unsigned bad = 0, shown = 0;
    for (unsigned n = 0; n < 200000; n++) {
        unsigned op = n % 5, mode = (n / 5) % 4;
        fpx a = random_operand(), b = random_operand();
        if (op == 3)        /* a sensible scale */
            b = X((rnd() & 0x8000) | (0x3FFF + (unsigned)(rnd() % 15)), rnd() | 1ull << 63);
        if (op == 1 || op == 2)     /* not too far apart: fprem loops */
            b.mantissa |= 1ull << 63, b.sign_exponent = (uint16_t)((b.sign_exponent & 0x8000)
                | ((a.sign_exponent & 0x7FFF) - (rnd() % 200) + 100));
        if ((b.sign_exponent & 0x7FFF) == 0 || (b.sign_exponent & 0x7FFF) >= 0x7FFF)
            continue;
        if ((a.sign_exponent & 0x7FFF) == 0 && op == 1)
            continue;
        fpx_env e = env(mode, 64);
        fpx r = X(0, 0);
        uint8_t q = 0;
        uint64_t conv = 0;
        switch (op) {
        case 0: r = fpx_int(&e, a, mode); break;
        case 1: r = fpx_rem(&e, a, b, true, &q); break;
        case 2: r = fpx_rem(&e, a, b, false, &q); break;
        case 3: r = fpx_scale(&e, a, b); break;
        default: conv = (n & 1) ? fpx_to_single(&e, a) : fpx_to_double(&e, a); break;
        }
        volatile long double x = to_ld(a), y = to_ld(b), z = 0;
        volatile float f = 0;
        volatile double d = 0;
        uint16_t sw = 0;
        feclearexcept(FE_ALL_EXCEPT);
        x87_control(mode, 64);
        switch (op) {
        case 0: __asm__ __volatile__("frndint" : "=t"(z) : "0"(x)); break;
        case 1:
            __asm__ __volatile__("1: fprem1\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b"
                                 : "=t"(z), "=a"(sw) : "0"(x), "u"(y));
            break;
        case 2:
            __asm__ __volatile__("1: fprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b"
                                 : "=t"(z), "=a"(sw) : "0"(x), "u"(y));
            break;
        case 3: __asm__ __volatile__("fscale" : "=t"(z) : "0"(x), "u"(y)); break;
        default:
            if (n & 1)
                f = (float)x;
            else
                d = (double)x;
            break;
        }
        x87_control(FPX_RN, 64);
        unsigned xf = x87_flags();
        unsigned mine = e.flags & ~(unsigned)FPX_UNFL;
        if ((e.flags & FPX_UNFL) && (e.flags & FPX_INEX2))
            mine |= FPX_UNFL;
        bool same;
        if (op == 4) {
            uint64_t want = 0;
            if (n & 1) {
                uint32_t u;
                memcpy(&u, (const void *)&f, 4);
                want = u;
            } else {
                memcpy(&want, (const void *)&d, 8);
            }
            same = conv == want;
            r = X(0, conv);
            z = 0;
        } else {
            fpx w = from_ld(z);
            if (fpx_is_inf(w))
                w.mantissa = 0;
            same = r.sign_exponent == w.sign_exponent && r.mantissa == w.mantissa;
            if (op == 1 || op == 2) {
                /* C0 C3 C1: quotient bits 2 1 0 */
                unsigned xq = ((sw >> 8) & 1) << 2 | ((sw >> 14) & 1) << 1 | ((sw >> 9) & 1);
                same = same && (q & 7) == xq;
                mine &= ~(unsigned)FPX_INEX2;   /* fprem's C1 is not inexact */
                xf &= ~(unsigned)FPX_INEX2;
            }
        }
        if (op == 0 || op == 1 || op == 2)
            xf &= ~(unsigned)FPX_UNFL, mine &= ~(unsigned)FPX_UNFL;
        if (!same || mine != xf) {
            bad++;
            if (shown++ < 10) {
                fpx w = from_ld(z);
                printf("    x87 more: op %u mode %u: %04x:%016llx, %04x:%016llx -> "
                       "%04x:%016llx q %02x %04x / x87 %04x:%016llx sw %04x %04x\n",
                       op, mode, a.sign_exponent, (unsigned long long)a.mantissa,
                       b.sign_exponent, (unsigned long long)b.mantissa,
                       r.sign_exponent, (unsigned long long)r.mantissa, q, mine,
                       w.sign_exponent, (unsigned long long)w.mantissa, sw, xf);
            }
        }
    }
    CHECK_EQ(bad, 0);
}

#else
void test_fpx_against_x87(void)
{
    /* x86-64 only */
}

void test_fpx_against_x87_more(void)
{
}
#endif

/* ---- Known values ------------------------------------------------------ */

void test_fpx_basic(void)
{
    fpx_env e = env(FPX_RN, 64);
    /* 1/3: 0.0101... rounds up in the last place; RZ does not */
    CHECK_X(fpx_div(&e, ONE, THREE), 0x3FFD, 0xAAAAAAAAAAAAAAABull);
    CHECK_EQ(e.flags, FPX_INEX2);
    e = env(FPX_RZ, 64);
    CHECK_X(fpx_div(&e, ONE, THREE), 0x3FFD, 0xAAAAAAAAAAAAAAAAull);
    e = env(FPX_RN, 24);
    CHECK_X(fpx_div(&e, ONE, THREE), 0x3FFD, 0xAAAAAB0000000000ull);
    e = env(FPX_RN, 53);
    CHECK_X(fpx_div(&e, ONE, THREE), 0x3FFD, 0xAAAAAAAAAAAAA800ull);
    /* 1 + 2^-63 is exact in extended, not in double */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_add(&e, ONE, X(0x3FFF - 63, 1ull << 63)), 0x3FFF,
            0x8000000000000001ull);
    CHECK_EQ(e.flags, 0);
    /* x - x is +0, -0 rounding toward minus infinity */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_sub(&e, THREE, THREE), 0x0000, 0);
    e = env(FPX_RM, 64);
    CHECK_X(fpx_sub(&e, THREE, THREE), 0x8000, 0);
    /* sqrt(2) */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_sqrt(&e, TWO), 0x3FFF, 0xB504F333F9DE6484ull);
    CHECK_EQ(e.flags, FPX_INEX2);
    /* sqrt(-1): OPERR and the default NaN; sqrt(-0) = -0 */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_sqrt(&e, X(0xBFFF, 1ull << 63)), 0x7FFF, ~0ull);
    CHECK_EQ(e.flags, FPX_OPERR);
    CHECK_X(fpx_sqrt(&e, X(0x8000, 0)), 0x8000, 0);
    /* inf - inf, 0 * inf, 0 / 0: OPERR; 1 / 0: DZ */
    e = env(FPX_RN, 64);
    fpx inf = X(0x7FFF, 0);
    CHECK(fpx_is_nan(fpx_sub(&e, inf, inf)));
    CHECK_EQ(e.flags, FPX_OPERR);
    e.flags = 0;
    CHECK_X(fpx_div(&e, ONE, X(0x8000, 0)), 0xFFFF, 0);
    CHECK_EQ(e.flags, FPX_DZ);
    /* NaNs: the destination wins, a signaling one is made quiet */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_add(&e, X(0x7FFF, 0xA000000000000001ull),
                    X(0xFFFF, 0xC000000000000002ull)),
            0x7FFF, 0xE000000000000001ull);
    CHECK_EQ(e.flags, FPX_SNAN);
    /* overflow: inf in RN, the largest number in RZ */
    fpx big = X(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    e = env(FPX_RN, 64);
    CHECK_X(fpx_mul(&e, big, TWO), 0x7FFF, 0);
    CHECK_EQ(e.flags, FPX_OVFL | FPX_INEX2);
    e = env(FPX_RZ, 64);
    CHECK_X(fpx_mul(&e, big, TWO), 0x7FFE, ~0ull);
    e = env(FPX_RM, 24);
    CHECK_X(fpx_mul(&e, big, TWO), 0x7FFE, 0xFFFFFF0000000000ull);
    /* underflow: an exact denormal raises UNFL but not INEX2 */
    e = env(FPX_RN, 64);
    fpx tiny = X(0x0001, 0x8000000000000000ull);     /* 2^-16382 */
    CHECK_X(fpx_div(&e, tiny, TWO), 0x0000, 0x4000000000000000ull);
    CHECK_EQ(e.flags, FPX_UNFL);
    CHECK_EQ(fpx_accrued(e.flags), 0);
    e.flags = 0;
    CHECK_X(fpx_mul(&e, X(0x0001, 0x8000000000000001ull), X(0x3FFE, 1ull << 63)),
            0x0000, 0x4000000000000000ull);     /* ties to even */
    CHECK_EQ(e.flags, FPX_UNFL | FPX_INEX2);
    CHECK_EQ(fpx_accrued(e.flags), 0x28);
    /* denormal and unnormalized operands are normalized */
    e = env(FPX_RN, 64);
    CHECK_X(fpx_add(&e, X(0x0000, 1ull << 62), X(0x0000, 1ull << 62)),
            0x0001, 0x8000000000000000ull);
    CHECK_X(fpx_round(&e, X(0x4000, 1ull << 61)), 0x3FFE,
            0x8000000000000000ull);
    CHECK_EQ(e.flags, 0);
}

void test_fpx_int_rem(void)
{
    fpx_env e = env(FPX_RN, 64);
    fpx two5 = X(0x4000, 0xA000000000000000ull), m25 = X(0xC000, 0xA000000000000000ull);
    CHECK_X(fpx_int(&e, two5, FPX_RN), 0x4000, 0x8000000000000000ull);   /* even */
    CHECK_X(fpx_int(&e, X(0x4000, 0xE000000000000000ull), FPX_RN),
            0x4001, 0x8000000000000000ull);                             /* 3.5 -> 4 */
    CHECK_X(fpx_int(&e, m25, FPX_RZ), 0xC000, 0x8000000000000000ull);
    CHECK_X(fpx_int(&e, m25, FPX_RM), 0xC000, 0xC000000000000000ull);
    CHECK_X(fpx_int(&e, X(0x3FFD, 1ull << 63), FPX_RZ), 0x0000, 0);   /* 0.25 */
    CHECK_X(fpx_int(&e, X(0xBFFD, 1ull << 63), FPX_RN), 0x8000, 0);   /* -0 */
    CHECK_X(fpx_int(&e, X(0x3FFD, 1ull << 63), FPX_RP), 0x3FFF, 1ull << 63);
    CHECK_EQ(e.flags, FPX_INEX2);

    uint8_t q;
    fpx ten = X(0x4002, 0xA000000000000000ull);
    e = env(FPX_RN, 64);
    CHECK_X(fpx_rem(&e, ten, THREE, true, &q), 0x3FFF, 1ull << 63);   /* 10 rem 3 = 1 */
    CHECK_EQ(q, 3);
    CHECK_X(fpx_rem(&e, X(0xC002, 0xA000000000000000ull), THREE, false, &q),
            0xBFFF, 1ull << 63);                                        /* -10 mod 3 = -1 */
    CHECK_EQ(q, 0x83);
    CHECK_X(fpx_rem(&e, X(0x4002, 0xB000000000000000ull), TWO, true, &q),
            0xBFFF, 1ull << 63);                                        /* 11 rem 2 = -1 */
    CHECK_EQ(q, 6);
    CHECK_X(fpx_rem(&e, X(0x4002, 0xA000000000000000ull), X(0x4001, 0xA000000000000000ull),
                    true, &q), 0x0000, 0);                              /* 10 rem 5 = +0 */
    CHECK_X(fpx_rem(&e, THREE, X(0x4001, 0x8000000000000000ull), true, &q),
            0xBFFF, 1ull << 63);                                        /* 3 rem 4 = -1 */
    CHECK_EQ(q, 1);
    CHECK_X(fpx_rem(&e, THREE, X(0x4001, 0x8000000000000000ull), false, &q),
            0x4000, 0xC000000000000000ull);                             /* 3 mod 4 = 3 */
    CHECK_EQ(q, 0);
    CHECK_EQ(e.flags, 0);
    CHECK(fpx_is_nan(fpx_rem(&e, ONE, X(0, 0), true, &q)));
    CHECK_EQ(e.flags, FPX_OPERR);

    e = env(FPX_RN, 64);
    CHECK_X(fpx_getexp(&e, ten), 0x4000, 0xC000000000000000ull);       /* 3 */
    CHECK_X(fpx_getman(&e, ten), 0x3FFF, 0xA000000000000000ull);       /* 1.25 */
    CHECK_X(fpx_getexp(&e, X(0x0000, 1)), 0xC00D, 0x807A000000000000ull); /* -16445 */
    CHECK_X(fpx_scale(&e, THREE, ten), 0x400A, 0xC000000000000000ull); /* 3 * 2^10 */
    CHECK_X(fpx_scale(&e, THREE, X(0xC002, 0xA800000000000000ull)),
            0x3FF6, 0xC000000000000000ull);                             /* 3 * 2^-10 */
    CHECK_EQ(e.flags, 0);
}

void test_fpx_conversions(void)
{
    fpx_env e = env(FPX_RN, 64);
    CHECK_X(fpx_from_single(0x3F800000), 0x3FFF, 1ull << 63);
    CHECK_X(fpx_from_single(0x00000001), 0x3FFF - 149, 1ull << 63);
    CHECK_X(fpx_from_single(0xFF800000), 0xFFFF, 0);
    CHECK_X(fpx_from_double(0x3FF8000000000000ull), 0x3FFF, 0xC000000000000000ull);
    CHECK_X(fpx_from_double(0x0000000000000001ull), 0x3FFF - 1074, 1ull << 63);
    CHECK_EQ(fpx_to_single(&e, ONE), 0x3F800000);
    CHECK_EQ(e.flags, 0);
    /* 1/3 to single: 0x3EAAAAAB; a tiny value to a single denormal */
    fpx third = fpx_div(&e, ONE, THREE);
    e.flags = 0;
    CHECK_EQ(fpx_to_single(&e, third), 0x3EAAAAAB);
    CHECK_EQ(fpx_to_double(&e, third), 0x3FD5555555555555ull);
    CHECK_EQ(e.flags, FPX_INEX2);
    e.flags = 0;
    CHECK_EQ(fpx_to_single(&e, X(0x3FFF - 140, 0xC000000000000000ull)), 0x00000300);
    CHECK_EQ(e.flags, FPX_UNFL);
    e.flags = 0;
    CHECK_EQ(fpx_to_single(&e, X(0x3FFF + 200, 1ull << 63)), 0x7F800000);
    CHECK_EQ(e.flags, FPX_OVFL | FPX_INEX2);
    /* a signaling NaN out: SNAN, quiet */
    e.flags = 0;
    CHECK_EQ(fpx_to_single(&e, X(0x7FFF, 0xA000000000000000ull)), 0x7FE00000);
    CHECK_EQ(e.flags, FPX_SNAN);
    /* integers: round with the mode, OPERR out of range */
    e = env(FPX_RN, 64);
    CHECK_EQ(fpx_to_int(&e, X(0x4000, 0xA000000000000000ull), 4), 2);
    CHECK_EQ(e.flags, FPX_INEX2);
    e = env(FPX_RN, 64);
    CHECK_EQ((uint32_t)fpx_to_int(&e, X(0x401E, 1ull << 63), 4), 0x7FFFFFFF);
    CHECK_EQ(e.flags, FPX_OPERR);
    e = env(FPX_RN, 64);
    CHECK_EQ((uint32_t)fpx_to_int(&e, X(0xC01E, 1ull << 63), 4), 0x80000000);
    CHECK_EQ(e.flags, 0);
    CHECK_EQ(fpx_to_int(&e, X(0xC006, 1ull << 63), 1), -128);
    CHECK_EQ(fpx_to_int(&e, X(0x4006, 1ull << 63), 1), 127);
    CHECK_EQ(e.flags, FPX_OPERR);
    e = env(FPX_RN, 24);    /* the precision does not apply here */
    CHECK_EQ(fpx_to_int(&e, X(0x401D, 0xFFFFFFFE00000000ull), 4), 0x7FFFFFFF);
    CHECK_EQ(e.flags, 0);
    /* FMOVE.L #0x7FFFFFFF in single precision rounds the mantissa */
    e = env(FPX_RN, 24);
    CHECK_X(fpx_from_int(&e, 0x7FFFFFFF), 0x401E, 0x8000000000000000ull);
    CHECK_EQ(e.flags, FPX_INEX2);
}

void test_fpx_packed(void)
{
    fpx_env e = env(FPX_RN, 64);
    uint32_t p[3];
    /* 1.2345E+10 */
    const uint32_t in1[3] = { 0x00100001, 0x23450000, 0 };
    CHECK_X(fpx_from_packed(&e, in1), 0x4020, 0xB7F4701000000000ull);
    CHECK_EQ(e.flags, 0);
    /* 1.3333333333333333: inexact, INEX1 */
    const uint32_t in2[3] = { 0x00000001, 0x33333333, 0x33333333 };
    CHECK_X(fpx_from_packed(&e, in2), 0x3FFF, 0xAAAAAAAAAAAAA977ull);
    CHECK_EQ(e.flags, FPX_INEX1);
    /* -1.23E-123 */
    e = env(FPX_RN, 64);
    const uint32_t in3[3] = { 0xC1230001, 0x23000000, 0 };
    fpx v = fpx_from_packed(&e, in3);
    CHECK_X(v, 0xBE66, 0xD0273EE52398B29Cull);
    /* pi with k = 5, -3, 17 and 18 (OPERR, 17 digits) */
    fpx pi = X(0x4000, 0xC90FDAA22168C235ull);
    e = env(FPX_RN, 64);
    fpx_to_packed(&e, pi, 5, p);
    CHECK_EQ(p[0], 0x00000003);
    CHECK_EQ(p[1], 0x14160000);
    CHECK_EQ(p[2], 0);
    CHECK_EQ(e.flags, FPX_INEX2);
    fpx_to_packed(&e, pi, -3, p);
    CHECK_EQ(p[1], 0x14200000);
    fpx_to_packed(&e, pi, 17, p);
    CHECK_EQ(p[1], 0x14159265);
    CHECK_EQ(p[2], 0x35897932);
    e.flags = 0;
    fpx_to_packed(&e, pi, 18, p);
    CHECK_EQ(p[2], 0x35897932);
    CHECK_EQ(e.flags, FPX_OPERR | FPX_INEX2);
    /* 123.456 with k = -2: 1.2346E+02; RZ: 1.2345E+02 */
    e = env(FPX_RN, 64);
    fpx x = fpx_from_packed(&e, (const uint32_t[3]){ 0x00020001, 0x23456000, 0 });
    fpx_to_packed(&e, x, -2, p);
    CHECK_EQ(p[0], 0x00020001);
    CHECK_EQ(p[1], 0x23460000);
    e = env(FPX_RZ, 64);
    fpx_to_packed(&e, x, -2, p);
    CHECK_EQ(p[1], 0x23450000);
    /* 9.99 with 2 digits: 1.0E+01 */
    e = env(FPX_RN, 64);
    x = fpx_from_packed(&e, (const uint32_t[3]){ 0x00000009, 0x99000000, 0 });
    fpx_to_packed(&e, x, 2, p);
    CHECK_EQ(p[0], 0x00010001);
    CHECK_EQ(p[1], 0);
    /* -1E-20, zero, infinity */
    x = fpx_from_packed(&e, (const uint32_t[3]){ 0xC0200001, 0, 0 });
    fpx_to_packed(&e, x, 3, p);
    CHECK_EQ(p[0], 0xC0200001);
    fpx_to_packed(&e, X(0x8000, 0), 3, p);
    CHECK_EQ(p[0], 0x80000000);
    fpx_to_packed(&e, X(0x7FFF, 0), 3, p);
    CHECK_EQ(p[0], 0x7FFF0000);
    /* the largest extended: 1.18973149535723177E+4932, 4 exponent digits */
    e = env(FPX_RN, 64);
    fpx_to_packed(&e, X(0x7FFE, ~0ull), 17, p);
    CHECK_EQ(p[0], 0x09324001);
    CHECK_EQ(p[1], 0x18973149);
    CHECK_EQ(p[2], 0x53572318);
    CHECK_EQ(e.flags, FPX_OPERR | FPX_INEX2);
}

void test_fpx_constants(void)
{
    fpx_env e = env(FPX_RN, 64);
    fpx v;
    CHECK(fpx_constant(&e, 0x00, &v));
    CHECK_X(v, 0x4000, 0xC90FDAA22168C235ull);          /* pi */
    CHECK(fpx_constant(&e, 0x0C, &v));
    CHECK_X(v, 0x4000, 0xADF85458A2BB4A9Aull);          /* e, as the ROM */
    CHECK(fpx_constant(&e, 0x30, &v));
    CHECK_X(v, 0x3FFE, 0xB17217F7D1CF79ACull);          /* ln 2 */
    CHECK(fpx_constant(&e, 0x3F, &v));
    CHECK_X(v, 0x7525, 0xC46052028A20979Bull);          /* 10^4096 */
    CHECK_EQ(e.flags, 0);
    CHECK(!fpx_constant(&e, 0x01, &v));
    CHECK(!fpx_constant(&e, 0x40, &v));
    e = env(FPX_RZ, 64);
    fpx_constant(&e, 0x00, &v);
    CHECK_X(v, 0x4000, 0xC90FDAA22168C234ull);
    e = env(FPX_RP, 64);
    fpx_constant(&e, 0x0C, &v);
    CHECK_X(v, 0x4000, 0xADF85458A2BB4A9Bull);
    e = env(FPX_RN, 24);
    fpx_constant(&e, 0x00, &v);
    CHECK_X(v, 0x4000, 0xC90FDB0000000000ull);
    CHECK_EQ(e.flags, FPX_INEX2);
}

void test_fpx_transcendental(void)
{
    fpx_env e = env(FPX_RN, 64);
    fpx zero = X(0, 0), mzero = X(0x8000, 0), inf = X(0x7FFF, 0);
    CHECK_X(fpx_trans(&e, FPX_SIN, mzero), 0x8000, 0);
    CHECK_X(fpx_trans(&e, FPX_COS, zero), 0x3FFF, 1ull << 63);
    CHECK_X(fpx_trans(&e, FPX_ETOX, X(0xFFFF, 0)), 0, 0);
    CHECK_X(fpx_trans(&e, FPX_ETOXM1, X(0xFFFF, 0)), 0xBFFF, 1ull << 63);
    CHECK_X(fpx_trans(&e, FPX_TWOTOX, TWO), 0x4001, 1ull << 63);
    CHECK_X(fpx_trans(&e, FPX_LOG2, X(0x4005, 1ull << 63)), 0x4001, 0xC000000000000000ull);
    CHECK_X(fpx_trans(&e, FPX_LOG10, X(0x4005, 0xC800000000000000ull)), 0x4000, 1ull << 63);
    CHECK_X(fpx_trans(&e, FPX_TENTOX, THREE), 0x4008, 0xFA00000000000000ull);
    CHECK_X(fpx_trans(&e, FPX_ACOS, ONE), 0, 0);
    CHECK_X(fpx_trans(&e, FPX_TANH, X(0xFFFF, 0)), 0xBFFF, 1ull << 63);
    CHECK_EQ(e.flags, 0);
    CHECK(fpx_is_nan(fpx_trans(&e, FPX_SIN, inf)));
    CHECK_EQ(e.flags, FPX_OPERR);
    e.flags = 0;
    CHECK_X(fpx_trans(&e, FPX_LOGN, zero), 0xFFFF, 0);
    CHECK_EQ(e.flags, FPX_DZ);
    e.flags = 0;
    CHECK_X(fpx_trans(&e, FPX_ATANH, X(0xBFFF, 1ull << 63)), 0xFFFF, 0);
    CHECK_EQ(e.flags, FPX_DZ);
    e.flags = 0;
    CHECK(fpx_is_nan(fpx_trans(&e, FPX_ASIN, TWO)));
    CHECK(fpx_is_nan(fpx_trans(&e, FPX_LOGNP1, X(0xC000, 1ull << 63))));
    CHECK_EQ(e.flags, FPX_OPERR);
    /* overflow of etox: the mode's result */
    e = env(FPX_RZ, 64);
    CHECK_X(fpx_trans(&e, FPX_ETOX, X(0x400D, 1ull << 63)), 0x7FFE, ~0ull);
    CHECK_EQ(e.flags, FPX_OVFL | FPX_INEX2);
#if LDBL_MANT_DIG == 64
    /* within one unit in the last place of the true values:
     * sin(1) = 0.d76aa47848677020c6e9e..., e = 2.b7e151628aed2a6abf7158... */
    e = env(FPX_RN, 64);
    fpx s1 = fpx_trans(&e, FPX_SIN, ONE);
    CHECK_EQ(s1.sign_exponent, 0x3FFE);
    CHECK(s1.mantissa - 0xD76AA47848677020ull <= 1);
    fpx ex = fpx_trans(&e, FPX_ETOX, ONE);
    CHECK_EQ(ex.sign_exponent, 0x4000);
    CHECK(ex.mantissa - 0xADF85458A2BB4A9Aull <= 1);
    fpx at = fpx_trans(&e, FPX_ATAN, ONE);          /* pi/4 */
    CHECK_EQ(at.sign_exponent, 0x3FFE);
    CHECK(at.mantissa - 0xC90FDAA22168C234ull <= 1);
    CHECK_EQ(e.flags, FPX_INEX2);
#endif
}

/* ---- Instructions ------------------------------------------------------- */

#define MEM_SIZE 0x40000u
#define CODE 0x1000u
#define SSP 0x30000u
#define DATA 0x8000u
#define HANDLER(v) (0x800u + 4u * (v))

static uint8_t mem[MEM_SIZE];
static relevo_cpu *cpu;
static relevo_regs *r;

static void put16(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}

static void put32(uint32_t a, uint32_t v)
{
    put16(a, v >> 16);
    put16(a + 2, v & 0xFFFF);
}

static uint32_t get16(uint32_t a)
{
    return (uint32_t)mem[a] << 8 | mem[a + 1];
}

static uint32_t get32(uint32_t a)
{
    return get16(a) << 16 | get16(a + 2);
}

/* A CPU of `model` with `fpu`, supervisor mode, at CODE. */
static void start(relevo_model model, relevo_fpu fpu, const uint16_t *code,
                  size_t n)
{
    memset(mem, 0, sizeof mem);
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.fpu = fpu;
    cfg.memory = mem;
    cfg.memory_size = sizeof mem;
    cpu = relevo_create(&cfg);
    CHECK(cpu != NULL);
    put32(0, SSP);
    put32(4, CODE);
    for (unsigned v = 2; v < 64; v++)
        put32(v * 4, HANDLER(v));
    for (uint32_t a = HANDLER(0); a < HANDLER(64); a += 2)
        put16(a, 0x4E71);
    for (size_t i = 0; i < n; i++)
        put16(CODE + 2 * (uint32_t)i, code[i]);
    relevo_reset(cpu);
    r = relevo_get_regs(cpu);
}

#define START(model, fpu, ...)                                          \
    do {                                                                \
        static const uint16_t code_[] = { __VA_ARGS__ };                \
        start(model, fpu, code_, sizeof code_ / sizeof code_[0]);       \
    } while (0)

#define START20(...) START(RELEVO_68020, RELEVO_FPU_68882, __VA_ARGS__)

static void step(void)
{
    CHECK_EQ(relevo_run(cpu, 1), RELEVO_BUDGET);
}

static void steps(unsigned n)
{
    while (n--)
        step();
}

static void done(void)
{
    relevo_destroy(cpu);
}

/* The CPU took `vector` and stacked `pc` and the vector offset. */
static void check_exception(unsigned vector, uint32_t pc)
{
    CHECK_EQ(r->pc, HANDLER(vector));
    CHECK_EQ(get32(r->a[7] + 2), pc);
    CHECK_EQ(get16(r->a[7] + 6) & 0x0FFF, vector * 4);
}

void test_fpu_table_no_conflicts(void)
{
    relevo_handler *t = malloc(sizeof *t * 0x10000);
    CHECK(t != NULL);
    if (t == NULL)
        return;
    CHECK_EQ(interp_fill_table(t, RELEVO_68020, RELEVO_FPU_68881), 0);
    CHECK_EQ(interp_fill_table(t, RELEVO_68030, RELEVO_FPU_68882), 0);
    CHECK_EQ(interp_fill_table(t, RELEVO_68040, RELEVO_FPU_INTERNAL), 0);
    CHECK_EQ(interp_fill_table(t, RELEVO_68060, RELEVO_FPU_INTERNAL), 0);
    free(t);
}

void test_fpu_reset_state(void)
{
    START20(0x4E71);
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(r->fp[i].sign_exponent, 0x7FFF);
        CHECK_EQ(r->fp[i].mantissa, ~0ull);
    }
    CHECK_EQ(r->fpcr, 0);
    CHECK_EQ(r->fpsr, 0);
    CHECK_EQ(r->fpiar, 0);
    done();
}

void test_fpu_move_and_arith(void)
{
    START20(0xF23C, 0x4000, 0x0000, 0x0003,     /* fmove.l #3,fp0 */
            0xF23C, 0x4080, 0x0000, 0x0001,     /* fmove.l #1,fp1 */
            0xF200, 0x00A0,                     /* fdiv.x fp0,fp1 */
            0xF200, 0x6480,                     /* fmove.s fp1,d0 */
            0xF210, 0x7480,                     /* fmove.d fp1,(a0) */
            0xF221, 0x6880,                     /* fmove.x fp1,-(a1) */
            0xF201, 0xA800,                     /* fmove.l fpsr,d1 */
            0xF202, 0xA400);                    /* fmove.l fpiar,d2 */
    r->a[0] = DATA;
    r->a[1] = DATA + 0x40;
    steps(3);
    CHECK_EQ(r->fp[1].sign_exponent, 0x3FFD);
    CHECK_EQ(r->fp[1].mantissa, 0xAAAAAAAAAAAAAAABull);
    CHECK_EQ(r->fpsr, 0x00000208);      /* INEX2, accrued INEX */
    CHECK_EQ(r->fpiar, CODE + 16);
    steps(5);
    CHECK_EQ(r->d[0], 0x3EAAAAAB);
    CHECK_EQ(get32(DATA), 0x3FD55555);
    CHECK_EQ(get32(DATA + 4), 0x55555555);
    CHECK_EQ(r->a[1], DATA + 0x40 - 12);
    CHECK_EQ(get32(DATA + 0x34), 0x3FFD0000);
    CHECK_EQ(get32(DATA + 0x38), 0xAAAAAAAA);
    CHECK_EQ(get32(DATA + 0x3C), 0xAAAAAAAB);
    CHECK_EQ(r->d[1], 0x00000008);      /* the exact FMOVE.X cleared INEX2 */
    CHECK_EQ(r->d[2], CODE + 28);       /* the FMOVE.X out */
    CHECK_EQ(r->pc, CODE + 40);
    done();
}

void test_fpu_operands(void)
{
    START20(0xF218, 0x5022,                     /* fadd.w (a0)+,fp0 */
            0xF23C, 0x5828, 0x00FE,             /* fsub.b #-2,fp0 */
            0xF228, 0x5423, 0x0008,             /* fmul.d 8(a0),fp0 */
            0xF210, 0x6C85,                     /* fmove.p fp1,(a0){#5} */
            0xF210, 0x7CB0,                     /* fmove.p fp1,(a0){d3} */
            0xF210, 0x4D00,                     /* fmove.p (a0),fp2 */
            0xF200, 0x6000,                     /* fmove.l fp0,d0 */
            0xF200, 0x7000,                     /* fmove.w fp0,d0 */
            0xF200, 0x7800);                    /* fmove.b fp0,d0 */
    r->fp[0] = X(0, 0);
    r->a[0] = DATA;
    put16(DATA, 0xFFF9);                        /* -7 */
    put32(DATA + 10, 0x40080000);               /* 3.0 (double) */
    put32(DATA + 14, 0);
    step();
    CHECK_EQ(r->a[0], DATA + 2);
    CHECK_EQ(r->fp[0].sign_exponent, 0xC001);   /* -7 */
    CHECK_EQ(r->fp[0].mantissa, 0xE000000000000000ull);
    CHECK_EQ(r->fpsr, FPX_CC_N);
    step();                                     /* -7 - -2 = -5 */
    CHECK_EQ(r->fp[0].mantissa, 0xA000000000000000ull);
    step();                                     /* -15 */
    CHECK_EQ(r->fp[0].sign_exponent, 0xC002);
    CHECK_EQ(r->fp[0].mantissa, 0xF000000000000000ull);
    r->fp[1] = X(0x4000, 0xC90FDAA22168C235ull);   /* pi */
    r->a[0] = DATA;
    step();
    CHECK_EQ(get32(DATA), 0x00000003);
    CHECK_EQ(get32(DATA + 4), 0x14160000);
    CHECK_EQ(r->fpsr & 0xFF00, FPX_INEX2);
    r->d[3] = 0x7D;                             /* k = -3 */
    step();
    CHECK_EQ(get32(DATA + 4), 0x14200000);
    step();                                     /* 3.142 back */
    CHECK_EQ(r->fp[2].sign_exponent, 0x4000);
    CHECK_EQ(r->fp[2].mantissa, 0xC916872B020C49BAull);
    CHECK_EQ(r->fpsr & 0xFF00, FPX_INEX1);
    r->d[0] = 0x12345678;
    step();
    CHECK_EQ(r->d[0], 0xFFFFFFF1);
    r->d[0] = 0x12345678;
    step();
    CHECK_EQ(r->d[0], 0x1234FFF1);
    step();
    CHECK_EQ(r->d[0], 0x1234FFF1);
    done();

    /* An is not a source; .X is not in Dn: line F */
    START20(0xF208, 0x4000,                     /* fmove.l a0,fp0 */
            0xF200, 0x4800);                    /* fmove.x d0,fp0 */
    step();
    check_exception(11, CODE);
    r->pc = CODE + 4;
    r->a[7] = SSP;
    step();
    check_exception(11, CODE + 4);
    done();
}

void test_fpu_fmovem(void)
{
    START20(0xF227, 0xE007,                     /* fmovem.x fp0-fp2,-(a7) */
            0xF21F, 0xD01C,                     /* fmovem.x (a7)+,fp3-fp5 */
            0xF227, 0xE800,                     /* fmovem.x d0,-(a7) */
            0xF21F, 0xD800,                     /* fmovem.x (a7)+,d0 */
            0xF220, 0xBC00,                     /* fmovem.l fpcr/fpsr/fpiar,-(a0) */
            0xF218, 0x9C00,                     /* fmovem.l (a0)+,fpcr/fpsr/fpiar */
            0xF200, 0x9000,                     /* fmove.l d0,fpcr */
            0xF208, 0x8400);                    /* fmove.l a0,fpiar */
    r->fp[0] = X(0x3FFF, 1ull << 63);
    r->fp[1] = X(0x0000, 5);                    /* moved as it is */
    r->fp[2] = X(0x7FFF, 0xA000000000000000ull); /* a signaling NaN too */
    step();
    CHECK_EQ(r->a[7], SSP - 36);
    CHECK_EQ(get32(SSP - 36), 0x3FFF0000);
    CHECK_EQ(get32(SSP - 24), 0x00000000);
    CHECK_EQ(get32(SSP - 16), 5);
    CHECK_EQ(get32(SSP - 12), 0x7FFF0000);
    step();
    CHECK_EQ(r->a[7], SSP);
    CHECK_EQ(r->fp[3].sign_exponent, 0x3FFF);
    CHECK_EQ(r->fp[4].mantissa, 5);
    CHECK_EQ(r->fp[5].mantissa, 0xA000000000000000ull);
    CHECK_EQ(r->fpsr, 0);                       /* no conversion, no flags */
    r->d[0] = 0x81;                             /* predecrement: FP0, FP7 */
    r->fp[7] = X(0x4000, 1ull << 63);
    step();
    CHECK_EQ(r->a[7], SSP - 24);
    CHECK_EQ(get32(SSP - 24), 0x3FFF0000);
    CHECK_EQ(get32(SSP - 12), 0x40000000);
    r->d[0] = 0x01;                             /* postincrement: FP7 */
    step();
    CHECK_EQ(r->a[7], SSP - 12);
    CHECK_EQ(r->fp[7].sign_exponent, 0x3FFF);
    r->fpcr = 0x00003430;
    r->fpsr = 0x0F00FF00;
    r->fpiar = 0x12345678;
    r->a[0] = DATA + 12;
    step();
    CHECK_EQ(r->a[0], DATA);
    CHECK_EQ(get32(DATA), 0x00003430);
    CHECK_EQ(get32(DATA + 4), 0x0F00FF00);
    CHECK_EQ(get32(DATA + 8), 0x12345678);
    put32(DATA, 0xFFFFFFFF);
    put32(DATA + 4, 0xFFFFFFFF);
    step();
    CHECK_EQ(r->a[0], DATA + 12);
    CHECK_EQ(r->fpcr, 0x0000FFF0);
    CHECK_EQ(r->fpsr, 0x0FFFFFF8);
    r->d[0] = 0x10;
    step();
    CHECK_EQ(r->fpcr, 0x10);
    r->a[0] = 0xCAFE;
    step();
    CHECK_EQ(r->fpiar, 0xCAFE);
    done();
}

void test_fpu_conditions(void)
{
    START20(0xF200, 0x0438,                     /* fcmp.x fp1,fp0 */
            0xF292, 0x0006,                     /* fbgt.w *+8 */
            0x4E71, 0x4E71,
            0xF240, 0x0012,                     /* fsgt d0 */
            0xF241, 0x0019,                     /* fsngl d1 */
            0xF248, 0x0001, 0xFFF8);            /* fdbeq d0,*-4 */
    r->fp[0] = THREE;
    r->fp[1] = TWO;
    step();
    CHECK_EQ(r->fpsr, 0);
    step();
    CHECK_EQ(r->pc, CODE + 12);                 /* taken */
    CHECK_EQ(r->fpiar, CODE + 4);
    r->d[0] = 0x12345600;
    step();
    CHECK_EQ(r->d[0], 0x123456FF);
    step();                                     /* NGL: not taken */
    CHECK_EQ(r->d[1] & 0xFF, 0);
    r->d[0] = 1;
    step();                                     /* not EQ: count down to 0 */
    CHECK_EQ(r->d[0], 0);
    CHECK_EQ(r->pc, CODE + 16);
    step();                                     /* FSNGL again */
    step();                                     /* to -1: falls through */
    CHECK_EQ(r->d[0], 0xFFFF);
    CHECK_EQ(r->pc, CODE + 26);
    done();

    /* A NaN: FBGT (not IEEE aware) sets BSUN, not taken; with BSUN
     * enabled it takes vector 48 before the branch. */
    START20(0xF200, 0x0438,                     /* fcmp.x fp1,fp0 */
            0xF292, 0x0006,                     /* fbgt.w *+8 */
            0xF282, 0x0006);                    /* fbogt.w *+8 */
    r->fp[0] = THREE;
    step();
    CHECK_EQ(r->fpsr, FPX_CC_NAN);
    step();
    CHECK_EQ(r->pc, CODE + 8);
    CHECK_EQ(r->fpsr, FPX_CC_NAN | FPX_BSUN | 0x80);
    r->fpsr = FPX_CC_NAN;
    step();                                     /* FBOGT: no BSUN */
    CHECK_EQ(r->fpsr, FPX_CC_NAN);
    r->pc = CODE + 4;
    r->fpcr = FPX_BSUN;
    step();
    check_exception(48, CODE + 4);
    done();

    /* FTRAPcc: vector 7, format 2 with the instruction's address */
    START20(0xF27C, 0x0001,                     /* ftrapeq */
            0xF27A, 0x000E, 0x0001,             /* ftrapne.w #1 */
            0xF27B, 0x000E, 0x0000, 0x0001);    /* ftrapne.l #1 */
    step();                                     /* not equal */
    CHECK_EQ(r->pc, CODE + 4);
    r->fpsr = FPX_CC_Z;
    step();                                     /* equal */
    CHECK_EQ(r->pc, CODE + 10);
    r->fpsr = 0;
    step();
    check_exception(7, CODE + 18);
    CHECK_EQ(get16(r->a[7] + 6) >> 12, 2);
    CHECK_EQ(get32(r->a[7] + 8), CODE + 10);
    done();
}

void test_fpu_exceptions(void)
{
    /* DZ enabled: the destination is kept, the exception waits for the
     * next FPU instruction (format 0, its address). */
    START20(0xF23C, 0x9000, 0x0000, 0x0400,     /* fmove.l #$400,fpcr */
            0xF200, 0x00A0,                     /* fdiv.x fp0,fp1 */
            0x4E71,
            0xF200, 0x003A,                     /* ftst.x fp0 */
            0xF200, 0x003A);
    r->fp[0] = X(0, 0);
    r->fp[1] = ONE;
    steps(2);
    CHECK_EQ(r->fp[1].sign_exponent, 0x3FFF);
    CHECK_EQ(r->fpsr & 0xFFFF, FPX_DZ | 0x10);
    step();
    CHECK_EQ(r->pc, CODE + 14);
    step();
    check_exception(50, CODE + 14);
    CHECK_EQ(get16(r->a[7] + 6) >> 12, 0);
    CHECK_EQ(r->fpiar, CODE + 8);
    r->pc = CODE + 14;
    step();                                     /* taken once */
    CHECK_EQ(r->pc, CODE + 18);
    done();

    /* OVFL and INEX enabled: the result is written; OVFL comes first */
    START20(0xF23C, 0x9000, 0x0000, 0x1200,     /* fmove.l #$1200,fpcr */
            0xF200, 0x00A3,                     /* fmul.x fp0,fp1 */
            0xF200, 0x003A);
    r->fp[0] = X(0x7FFE, 1ull << 63);
    r->fp[1] = TWO;
    steps(3);
    CHECK_EQ(r->fp[1].sign_exponent, 0x7FFF);
    check_exception(53, CODE + 12);
    done();

    /* FSAVE keeps the pending exception, FRESTORE brings it back */
    START20(0xF23C, 0x9000, 0x0000, 0x0400,     /* fmove.l #$400,fpcr */
            0xF200, 0x00A0,                     /* fdiv.x fp0,fp1 */
            0xF327,                             /* fsave -(a7) */
            0xF200, 0x003A,                     /* ftst.x fp0 */
            0xF35F,                             /* frestore (a7)+ */
            0xF200, 0x003A);                    /* ftst.x fp0 */
    r->fp[0] = X(0, 0);
    r->fp[1] = ONE;
    steps(3);
    CHECK_EQ(r->a[7], SSP - 60);
    CHECK_EQ(get32(SSP - 60), 0x1F380000);
    step();
    CHECK_EQ(r->pc, CODE + 18);                 /* nothing pending */
    step();
    CHECK_EQ(r->a[7], SSP);
    step();
    check_exception(50, CODE + 20);
    done();
}

void test_fpu_fsave_frestore(void)
{
    /* After a reset the frame is null; after an FPU instruction, idle */
    START20(0xF310,                             /* fsave (a0) */
            0xF200, 0x003A,                     /* ftst.x fp0 */
            0xF310,                             /* fsave (a0) */
            0xF350);                            /* frestore (a0) */
    r->a[0] = DATA;
    put32(DATA, 0xFFFFFFFF);
    step();
    CHECK_EQ(get32(DATA), 0);
    steps(2);
    CHECK_EQ(get32(DATA), 0x1F380000);
    put32(DATA, 0);                             /* a null frame resets */
    r->fpcr = 0x30;
    r->fp[3] = ONE;
    step();
    CHECK_EQ(r->fpcr, 0);
    CHECK_EQ(r->fp[3].sign_exponent, 0x7FFF);
    done();

    /* A bad frame is a format error; FSAVE is privileged */
    START20(0xF350,                             /* frestore (a0) */
            0xF310);                            /* fsave (a0) */
    r->a[0] = DATA;
    put32(DATA, 0x12340000);
    step();
    check_exception(14, CODE);
    r->pc = CODE + 2;
    r->a[7] = SSP;
    relevo_set_sr(cpu, 0x0000);
    r->a[7] = 0x20000;
    step();
    check_exception(8, CODE + 2);
    done();

    /* The 68881's idle frame is 28 bytes, the 68040's 4, the 68060's 12 */
    START(RELEVO_68030, RELEVO_FPU_68881, 0xF200, 0x003A, 0xF327);
    steps(2);
    CHECK_EQ(r->a[7], SSP - 28);
    CHECK_EQ(get32(SSP - 28), 0x1F180000);
    done();
    START(RELEVO_68040, RELEVO_FPU_INTERNAL, 0xF327, 0xF200, 0x003A, 0xF327);
    step();
    CHECK_EQ(r->a[7], SSP - 4);
    CHECK_EQ(get32(SSP - 4), 0);
    steps(2);
    CHECK_EQ(r->a[7], SSP - 8);
    CHECK_EQ(get32(SSP - 8), 0x41000000);
    done();
    START(RELEVO_68060, RELEVO_FPU_INTERNAL, 0xF200, 0x003A, 0xF327);
    steps(2);
    CHECK_EQ(r->a[7], SSP - 12);
    CHECK_EQ(get32(SSP - 12), 0x00006000);
    done();
}

void test_fpu_models(void)
{
    /* The 68040 runs FSADD and FDSQRT, not FSIN, FINT or FMOVECR */
    START(RELEVO_68040, RELEVO_FPU_INTERNAL,
          0xF200, 0x00E2,                       /* fsadd.x fp0,fp1 */
          0xF200, 0x00C5,                       /* fdsqrt.x fp0,fp1 */
          0xF200, 0x008E,                       /* fsin.x fp0,fp1 */
          0xF200, 0x0081,                       /* fint.x fp0,fp1 */
          0xF200, 0x5D80);                      /* fmovecr #0,fp3 */
    r->fp[0] = X(0x3FFF - 30, 1ull << 63);      /* 2^-30 */
    r->fp[1] = ONE;
    step();
    CHECK_EQ(r->fp[1].mantissa, 1ull << 63);    /* 1 + 2^-30 in single */
    CHECK_EQ(r->fpsr & 0xFF00, FPX_INEX2);
    r->fp[0] = TWO;
    step();
    CHECK_EQ(r->fp[1].mantissa, 0xB504F333F9DE6800ull);
    step();
    check_exception(11, CODE + 8);
    r->pc = CODE + 12;
    r->a[7] = SSP;
    step();
    check_exception(11, CODE + 12);
    r->pc = CODE + 16;
    r->a[7] = SSP;
    step();
    check_exception(11, CODE + 16);
    done();

    /* The 68060 runs FINT, not FScc */
    START(RELEVO_68060, RELEVO_FPU_INTERNAL,
          0xF200, 0x0081,                       /* fint.x fp0,fp1 */
          0xF240, 0x0012);                      /* fsgt d0 */
    r->fp[0] = X(0x4000, 0xA000000000000000ull);
    step();
    CHECK_EQ(r->fp[1].sign_exponent, 0x4000);
    CHECK_EQ(r->fp[1].mantissa, 1ull << 63);
    step();
    check_exception(11, CODE + 4);
    done();

    /* The 68881 has no FSADD; FMOVECR, FSINCOS and the rest are there */
    START(RELEVO_68020, RELEVO_FPU_68881,
          0xF200, 0x00E2,                       /* fsadd.x fp0,fp1 */
          0xF200, 0x5D80,                       /* fmovecr #0,fp3 */
          0xF200, 0x0131);                      /* fsincos.x fp0,fp1:fp2 */
    step();
    check_exception(11, CODE);
    r->pc = CODE + 4;
    step();
    CHECK_EQ(r->fp[3].mantissa, 0xC90FDAA22168C235ull);
    r->fp[0] = X(0, 0);
    step();
    CHECK_EQ(r->fp[2].mantissa, 0);             /* sin 0 */
    CHECK_EQ(r->fp[1].sign_exponent, 0x3FFF);   /* cos 0 */
    done();

    /* No FPU: line F */
    START(RELEVO_68020, RELEVO_FPU_NONE, 0xF200, 0x003A);
    step();
    check_exception(11, CODE);
    done();
}
