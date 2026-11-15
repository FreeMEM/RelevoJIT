// SPDX-License-Identifier: MPL-2.0
//! The transcendental functions of the 68881.
//!
//! The special operands (zeros, infinities, NaNs, out of the domain) and the
//! exact results are as the MC68881UM gives them (and as the C version,
//! `fpx_trans.c`, does). The rest is computed here, without the host's math
//! library, so the result is the same on every host: on a wide internal
//! format ([`W`]: a 128-bit mantissa and a 32-bit exponent, about 120 good
//! bits after the reductions), then rounded once with the environment, so
//! the FPCR's mode and precision act on the true value (the C version
//! rounds to nearest in `long double` first). With some 50 bits to spare
//! beyond the 64 of the result, it is correctly rounded except in
//! vanishingly rare cases. INEX2 is set unless the result is exact.
//!
//! Methods (all from the textbook identities, written for this format):
//!
//! - exp: x = k ln2 + r with ln2 in two parts (k ln2_hi is exact), exp(r)
//!   by its series on r / 256, squared eight times. expm1 on |x| < 1: the
//!   series on x / 256, then expm1(2y) = E (E + 2) eight times (no
//!   cancellation).
//! - log: x = 2^E m with m in [sqrt(1/2), sqrt(2)), log m = 2 atanh((m - 1)
//!   / (m + 1)) by its series; lognp1 near 0 as 2 atanh(x / (2 + x)).
//! - sin, cos, tan: exact reduction modulo pi/2 for any argument (the
//!   product with enough bits of 2/pi, a table computed offline with
//!   integer arithmetic), then the series on |r| <= pi/4.
//! - atan: atan(t) = atan(k/8) + atan((t - k/8) / (1 + t k/8)) with a
//!   table of atan(k/8), then the series; asin, acos and atanh through atan and lognp1; the hyperbolic
//!   functions through exp and expm1.

use super::{DZ, Env, Fpx, INEX2, OPERR, TOP};
use super::{default_nan, exp, is_inf, is_nan, is_snan, is_zero, make, pack, sign};

/// The functions, numbered as `fpx_func` of tests/hooks.h.
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Func {
    Sinh,
    Lognp1,
    Etoxm1,
    Tanh,
    Atan,
    Asin,
    Atanh,
    Sin,
    Tan,
    Etox,
    Twotox,
    Tentox,
    Logn,
    Log10,
    Log2,
    Cosh,
    Acos,
    Cos,
}

impl Func {
    /// The function numbered `n` (as `fpx_func`).
    pub fn from_u32(n: u32) -> Option<Func> {
        const ALL: [Func; 18] = [
            Func::Sinh,
            Func::Lognp1,
            Func::Etoxm1,
            Func::Tanh,
            Func::Atan,
            Func::Asin,
            Func::Atanh,
            Func::Sin,
            Func::Tan,
            Func::Etox,
            Func::Twotox,
            Func::Tentox,
            Func::Logn,
            Func::Log10,
            Func::Log2,
            Func::Cosh,
            Func::Acos,
            Func::Cos,
        ];
        ALL.get(n as usize).copied()
    }
}

// ---- The wide format -----------------------------------------------------

/// value = (-1)^neg * m * 2^(e - 127), m with bit 127 set, or m = 0 for a
/// zero. The convention of [`pack`] (hi:lo = m, exponent e).
#[derive(Clone, Copy, Debug)]
struct W {
    neg: bool,
    e: i32,
    m: u128,
}

const ZERO: W = W {
    neg: false,
    e: 0,
    m: 0,
};
const ONE: W = W {
    neg: false,
    e: 0,
    m: 1 << 127,
};
/// Just below 1: 1 - 2^-128, for results that round to or just below 1.
const ONE_MINUS: W = W {
    neg: false,
    e: -1,
    m: u128::MAX,
};

include!("trans_consts.rs");

impl W {
    fn norm(neg: bool, e: i32, m: u128) -> W {
        if m == 0 {
            return W { neg, e: 0, m: 0 };
        }
        let s = m.leading_zeros();
        W {
            neg,
            e: e - s as i32,
            m: m << s,
        }
    }

    fn from_fpx(a: Fpx) -> W {
        let mut m = a.mantissa;
        if m == 0 {
            return W {
                neg: sign(a),
                ..ZERO
            };
        }
        let ex = exp(a);
        let s = m.leading_zeros();
        m <<= s;
        let e = (if ex == 0 { 1 } else { ex as i32 }) - 16383 - s as i32;
        W {
            neg: sign(a),
            e,
            m: u128::from(m) << 64,
        }
    }

    fn from_i64(v: i64) -> W {
        W::norm(v < 0, 127, u128::from(v.unsigned_abs()))
    }

    fn is_zero(self) -> bool {
        self.m == 0
    }

    fn neg(self) -> W {
        W {
            neg: !self.neg,
            ..self
        }
    }

    fn abs(self) -> W {
        W { neg: false, ..self }
    }

    /// times 2^n
    fn ldexp(self, n: i32) -> W {
        if self.m == 0 {
            self
        } else {
            W {
                e: self.e + n,
                ..self
            }
        }
    }

    fn add(self, b: W) -> W {
        if self.m == 0 {
            return b;
        }
        if b.m == 0 {
            return self;
        }
        let (x, y) = if (self.e, self.m) >= (b.e, b.m) {
            (self, b)
        } else {
            (b, self)
        };
        let d = (x.e - y.e) as u32;
        if d >= 128 {
            return x;
        }
        let ym = y.m >> d;
        if x.neg == y.neg {
            let (s, carry) = x.m.overflowing_add(ym);
            if carry {
                return W {
                    neg: x.neg,
                    e: x.e + 1,
                    m: s >> 1 | 1 << 127,
                };
            }
            W {
                neg: x.neg,
                e: x.e,
                m: s,
            }
        } else {
            W::norm(x.neg, x.e, x.m - ym)
        }
    }

    fn sub(self, b: W) -> W {
        self.add(b.neg())
    }

    fn mul(self, b: W) -> W {
        let neg = self.neg != b.neg;
        if self.m == 0 || b.m == 0 {
            return W { neg, ..ZERO };
        }
        let (a1, a0) = ((self.m >> 64) as u64, self.m as u64);
        let (b1, b0) = ((b.m >> 64) as u64, b.m as u64);
        let p11 = u128::from(a1) * u128::from(b1);
        let p10 = u128::from(a1) * u128::from(b0);
        let p01 = u128::from(a0) * u128::from(b1);
        let p00 = u128::from(a0) * u128::from(b0);
        let mid = (p00 >> 64) + (p10 as u64 as u128) + (p01 as u64 as u128);
        let lo = (mid as u64 as u128) << 64 | (p00 as u64 as u128);
        let hi = p11 + (p10 >> 64) + (p01 >> 64) + (mid >> 64);
        // product = hi:lo * 2^(e1 + e2 - 254), top bit at 255 or 254
        if hi >> 127 != 0 {
            W {
                neg,
                e: self.e + b.e + 1,
                m: hi,
            }
        } else {
            W {
                neg,
                e: self.e + b.e,
                m: hi << 1 | lo >> 127,
            }
        }
    }

    fn sqr(self) -> W {
        self.mul(self)
    }

    /// self / b: the reciprocal of b from a 64-bit quotient and one Newton
    /// step (about 125 good bits).
    fn div(self, b: W) -> W {
        let neg = self.neg != b.neg;
        if self.m == 0 {
            return W { neg, ..ZERO };
        }
        // B = b's mantissa as a value in [1, 2); y ~ 1/B
        let bb = W {
            neg: false,
            e: 0,
            m: b.m,
        };
        let q = (1u128 << 127) / (b.m >> 64);
        let y = W::norm(false, 63, q); // q * 2^-64
        let two = W {
            neg: false,
            e: 1,
            m: 1 << 127,
        };
        let y = y.mul(two.sub(bb.mul(y)));
        let r = W {
            neg: false,
            e: self.e - b.e,
            m: self.m,
        }
        .mul(y);
        W { neg, ..r }
    }

    /// Division by a small positive integer.
    fn div_int(self, n: u64) -> W {
        if self.m == 0 {
            return self;
        }
        let n = u128::from(n);
        let q = self.m / n;
        let r = self.m % n;
        let s = q.leading_zeros();
        W {
            neg: self.neg,
            e: self.e - s as i32,
            m: (q << s) | ((r << s) / n),
        }
    }

    /// sqrt of a positive value: 64 bits from the integer root, then one
    /// Newton step.
    fn sqrt(self) -> W {
        if self.m == 0 {
            return self;
        }
        // value = m * 2^p; make p even
        let p = self.e - 127;
        let (rad, p) = if p & 1 != 0 {
            (self.m >> 1, p + 1)
        } else {
            (self.m, p)
        };
        // sqrt(rad * 2^p) = sqrt(rad) * 2^(p / 2)
        let (root, _) = super::isqrt128(rad);
        let g = W::norm(false, 127 + p / 2, u128::from(root));
        g.add(self.div(g)).ldexp(-1)
    }

    /// The nearest integer, for values well inside i64.
    fn round_i64(self) -> i64 {
        if self.m == 0 || self.e < -1 {
            return 0;
        }
        // m * 2^(e - 127): shift right by 127 - e, rounding to nearest
        let sh = (127 - self.e) as u32;
        let v = ((self.m >> (sh - 1)) + 1) >> 1;
        let v = v as i64;
        if self.neg { -v } else { v }
    }

    /// |self| == 1
    fn is_one(self) -> bool {
        self.e == 0 && self.m == 1 << 127
    }

    /// |self| > 1
    fn above_one(self) -> bool {
        self.m != 0 && (self.e > 0 || (self.e == 0 && self.m != 1 << 127))
    }

    /// Whether |self| < 2^n
    fn below(self, n: i32) -> bool {
        self.m == 0 || self.e < n
    }

    /// Rounds to the environment; `exact` false marks a value known not to
    /// be representable (the sticky bit), so INEX2 is always set.
    fn to_fpx(self, e: &mut Env) -> Fpx {
        if self.m == 0 {
            return zero(self.neg);
        }
        let (hi, lo) = ((self.m >> 64) as u64, self.m as u64);
        pack(e, self.neg, self.e, hi, lo | 1)
    }
}

/// The sum of a series, term by term, until the term no longer counts.
/// `next(term, k)` gives term k from term k - 1 (k from 1).
fn series(first: W, mut next: impl FnMut(W, u64) -> W) -> W {
    let mut sum = first;
    let mut term = first;
    for k in 1..200 {
        term = next(term, k);
        if term.is_zero() || term.e < sum.e - 130 {
            break;
        }
        sum = sum.add(term);
    }
    sum
}

// ---- Exponentials ----------------------------------------------------------

/// exp(x) for |x| < 2^14 (the result may be beyond the extended range: the
/// exponent here is wide).
fn exp_w(x: W) -> W {
    let k = x.mul(INV_LN2).round_i64();
    let kw = W::from_i64(k);
    let r = x.sub(kw.mul(LN2_HI)).sub(kw.mul(LN2_LO));
    let y = r.ldexp(-8);
    // exp(y) = 1 + y + y^2/2 + ...
    let mut v = series(ONE, |t, n| t.mul(y).div_int(n));
    for _ in 0..8 {
        v = v.sqr();
    }
    v.ldexp(k as i32)
}

/// exp(x) - 1 for |x| < 1, without cancellation.
fn expm1_small(x: W) -> W {
    let y = x.ldexp(-8);
    // y + y^2/2 + y^3/6 + ...
    let mut v = series(y, |t, n| t.mul(y).div_int(n + 1));
    for _ in 0..8 {
        v = v.mul(v.add(W::from_i64(2)));
    }
    v
}

/// exp(x) - 1 for |x| < 2^14.
fn expm1_w(x: W) -> W {
    if x.below(0) {
        expm1_small(x)
    } else if x.neg && x.e >= 6 {
        // -64 or below: -1 plus less than 2^-92, between the 64-bit
        // neighbours of -1 as 1 - 2^-128 is
        ONE_MINUS.neg()
    } else {
        exp_w(x).sub(ONE)
    }
}

// ---- Logarithms ------------------------------------------------------------

/// atanh(s) = s + s^3/3 + s^5/5 ..., for |s| <= 0.2.
fn atanh_series(s: W) -> W {
    let s2 = s.sqr();
    let mut pow = s;
    let mut sum = s;
    for k in 1..200u64 {
        pow = pow.mul(s2);
        let term = pow.div_int(2 * k + 1);
        if term.is_zero() || term.e < sum.e - 130 {
            break;
        }
        sum = sum.add(term);
    }
    sum
}

/// ln(x) for x > 0, as E ln2 + ln(m), and also E and ln(m) apart.
fn log_parts(x: W) -> (i32, W) {
    // x = m * 2^E with m in [1, 2)
    let mut big_e = x.e;
    let mut m = W {
        neg: false,
        e: 0,
        m: x.m,
    };
    // sqrt(2) = 1.0110101...: take m / 2 above it
    if x.m > 0xB504_F333_F9DE_6484_597D_89B3_754A_BE9F {
        m.e = -1;
        big_e += 1;
    }
    let s = m.sub(ONE).div(m.add(ONE));
    (big_e, atanh_series(s).ldexp(1))
}

fn log_w(x: W) -> W {
    let (big_e, lm) = log_parts(x);
    W::from_i64(i64::from(big_e)).mul(LN2).add(lm)
}

/// ln(1 + x) for x > -1.
fn log1p_w(x: W) -> W {
    if x.below(-2) {
        // |x| < 1/4: 2 atanh(x / (2 + x))
        let s = x.div(x.add(W::from_i64(2)));
        atanh_series(s).ldexp(1)
    } else {
        log_w(ONE.add(x))
    }
}

// ---- Trigonometry ------------------------------------------------------------

/// |x| = q pi/2 + r with |r| <= pi/4: (q mod 4, r). Exact for any x: the
/// fraction of x * 2/pi is taken from a product with the bits of 2/pi that
/// matter (Payne and Hanek's method).
fn reduce(x: W) -> (u32, W) {
    let x = x.abs();
    if x.below(-1) {
        // below 1/2 < pi/4
        return (0, x);
    }
    // x = mx * 2^(e - 63), mx a 64-bit integer (the operands have 64 bits)
    let mx = (x.m >> 64) as u64;
    let e = x.e;
    // Bits i0 .. i0 + 255 of 2/pi (bit i weighs 2^-i): the earlier ones
    // only add multiples of 4 to x * 2/pi.
    let i0 = (e - 64).max(1) as usize;
    let mut win = [0u64; 4];
    for (j, w) in win.iter_mut().enumerate() {
        // bits i0 + 64j .. i0 + 64j + 63
        let b = i0 - 1 + 64 * j; // index from 0 of the first bit
        let (word, off) = (b / 64, b % 64);
        let hi = TWO_OVER_PI.get(word).copied().unwrap_or(0);
        let lo = TWO_OVER_PI.get(word + 1).copied().unwrap_or(0);
        *w = if off == 0 {
            hi
        } else {
            hi << off | lo >> (64 - off)
        };
    }
    // P = mx * window: 320 bits, as five 64-bit limbs (most significant
    // first)
    let mut p = [0u64; 5];
    let mut carry: u128 = 0;
    for j in (0..4).rev() {
        let t = u128::from(mx) * u128::from(win[j]) + carry;
        p[j + 1] = t as u64;
        carry = t >> 64;
    }
    p[0] = carry as u64;
    // x * 2/pi = P * 2^(e - 63 - (i0 + 255)): `frac` fraction bits
    let frac = (i0 as i32 + 255 + 63 - e) as u32;
    // the two integer bits above the fraction, and the fraction as a
    // left-aligned 320-bit number
    let int_bits = 320 - frac;
    let get_bit = |i: u32| -> u64 {
        // bit i from the top of P (0 = msb)
        (p[(i / 64) as usize] >> (63 - i % 64)) & 1
    };
    let mut q = 0u32;
    if int_bits >= 2 {
        q = (get_bit(int_bits - 2) << 1 | get_bit(int_bits - 1)) as u32;
    } else if int_bits == 1 {
        q = get_bit(0) as u32;
    }
    // the fraction: bits int_bits .. 319, 256 of them taken, left-aligned
    let mut f = [0u64; 4];
    for (k, fk) in f.iter_mut().enumerate() {
        let mut v = 0u64;
        for b in 0..64 {
            let i = int_bits + 64 * k as u32 + b;
            v = v << 1 | if i < 320 { get_bit(i) } else { 0 };
        }
        *fk = v;
    }
    // f >= 1/2: the nearer multiple is the next one, r = f - 1
    let mut neg = false;
    if f[0] >> 63 != 0 {
        q = q.wrapping_add(1);
        neg = true;
        // 1 - f, as a 256-bit two's complement
        let mut borrow = true;
        for fk in f.iter_mut().rev() {
            let (v, b) = (!*fk).overflowing_add(u64::from(borrow));
            *fk = v;
            borrow = b;
        }
    }
    // the top 128 significant bits of the fraction
    let lead = f.iter().position(|&w| w != 0);
    let Some(lw) = lead else {
        return (q & 3, ZERO);
    };
    let s = f[lw].leading_zeros();
    let word = |k: usize| f.get(k).copied().unwrap_or(0);
    let take = |k: usize| -> u64 {
        if s == 0 {
            word(k)
        } else {
            word(k) << s | word(k + 1) >> (64 - s)
        }
    };
    let m = u128::from(take(lw)) << 64 | u128::from(take(lw + 1));
    // value = m * 2^-(64 lw + s + 128)... as W: top bit weight 2^-(64lw+s+1)
    let fw = W {
        neg,
        e: -(64 * lw as i32 + s as i32 + 1),
        m,
    };
    (q & 3, fw.mul(PI_2))
}

fn sin_series(r: W) -> W {
    let r2 = r.sqr();
    series(r, |t, n| t.mul(r2).div_int((2 * n) * (2 * n + 1)).neg())
}

fn cos_series(r: W) -> W {
    let r2 = r.sqr();
    series(ONE, |t, n| t.mul(r2).div_int((2 * n - 1) * (2 * n)).neg())
}

fn sin_w(x: W) -> W {
    let (q, r) = reduce(x);
    let v = match q {
        0 => sin_series(r),
        1 => cos_series(r),
        2 => sin_series(r).neg(),
        _ => cos_series(r).neg(),
    };
    if x.neg { v.neg() } else { v }
}

fn cos_w(x: W) -> W {
    let (q, r) = reduce(x);
    match q {
        0 => cos_series(r),
        1 => sin_series(r).neg(),
        2 => cos_series(r).neg(),
        _ => sin_series(r),
    }
}

fn tan_w(x: W) -> W {
    let (q, r) = reduce(x);
    let (s, c) = (sin_series(r), cos_series(r));
    let v = if q & 1 == 0 { s.div(c) } else { c.div(s).neg() };
    if x.neg { v.neg() } else { v }
}

/// atan(x) for any finite x: for |x| > 1, pi/2 - atan(1/|x|); then
/// atan(t) = atan(c) + atan((t - c) / (1 + t c)) with c the nearest k/8,
/// and the series on |u| <= 1/16.
fn atan_w(x: W) -> W {
    let a = x.abs();
    let big = a.above_one();
    let t = if big { ONE.div(a) } else { a };
    let k = t.ldexp(3).round_i64() as usize; // 0..=8
    let c = W::from_i64(k as i64).ldexp(-3);
    let u = t.sub(c).div(ONE.add(t.mul(c)));
    let u2 = u.sqr();
    let mut pow = u;
    let mut sum = u;
    for n in 1..100u64 {
        pow = pow.mul(u2).neg();
        let term = pow.div_int(2 * n + 1);
        if term.is_zero() || term.e < sum.e - 130 {
            break;
        }
        sum = sum.add(term);
    }
    let mut v = ATAN_K8[k].add(sum);
    if big {
        v = PI_2.sub(v);
    }
    if x.neg { v.neg() } else { v }
}

// ---- The functions ---------------------------------------------------------

fn operr(e: &mut Env) -> Fpx {
    e.flags |= OPERR;
    default_nan()
}

fn dz(e: &mut Env, sign: bool) -> Fpx {
    e.flags |= DZ;
    make(sign, 0x7FFF, 0)
}

fn zero(sign: bool) -> Fpx {
    make(sign, 0, 0)
}

fn one(sign: bool) -> Fpx {
    make(sign, 16383, TOP)
}

fn inf(sign: bool) -> Fpx {
    make(sign, 0x7FFF, 0)
}

/// Far beyond the range: the mode's overflow (or underflow) result, with
/// its flags.
fn overflow(e: &mut Env, sign: bool) -> Fpx {
    pack(e, sign, 0x10000, TOP, 0)
}

fn underflow(e: &mut Env, sign: bool) -> Fpx {
    pack(e, sign, -0x10000, TOP, 0)
}

/// An exact value given as an integer.
fn exact_int(e: &mut Env, v: i64) -> Fpx {
    if v == 0 {
        return zero(false);
    }
    pack(e, v < 0, 63, v.unsigned_abs(), 0)
}

/// Whether x is an integer of at most 100 000 in size, and which (the rule
/// of the C version for the exact results).
fn integral(x: W) -> Option<i64> {
    if x.is_zero() {
        return Some(0);
    }
    if x.e < 0 || x.e > 16 {
        return None;
    }
    let sh = (127 - x.e) as u32;
    if x.m & ((1u128 << sh) - 1) != 0 {
        return None;
    }
    let v = (x.m >> sh) as i64;
    if v > 100_000 {
        return None;
    }
    Some(if x.neg { -v } else { v })
}

/// 10^n exactly, n in 0..=27 (up to 90 bits; its odd part, 5^n, fits
/// the 64-bit mantissa).
fn pow10(n: i64) -> W {
    W::norm(false, 127, (0..n).fold(1u128, |v, _| v * 10))
}

/// The transcendental function `f` of `a`, rounded with the environment.
pub fn trans(e: &mut Env, f: Func, a: Fpx) -> Fpx {
    if is_nan(a) {
        if is_snan(a) {
            e.flags |= super::SNAN;
        }
        let mut q = a;
        q.mantissa |= super::QUIET;
        return q;
    }
    let s = sign(a);
    let isz = is_zero(a);
    let isi = is_inf(a);
    let x = W::from_fpx(a);
    // |x| > 1 (also for infinities)
    let above_one = isi || x.above_one();

    match f {
        Func::Sin | Func::Tan => {
            if isi {
                return operr(e);
            }
            if isz {
                return zero(s);
            }
            if f == Func::Sin {
                sin_w(x).to_fpx(e)
            } else {
                tan_w(x).to_fpx(e)
            }
        }
        Func::Cos => {
            if isi {
                return operr(e);
            }
            if isz {
                return one(false);
            }
            cos_w(x).to_fpx(e)
        }
        Func::Atan => {
            if isz {
                return zero(s);
            }
            if isi {
                return (if s { PI_2.neg() } else { PI_2 }).to_fpx(e);
            }
            atan_w(x).to_fpx(e)
        }
        Func::Asin | Func::Acos => {
            if above_one {
                return operr(e);
            }
            if f == Func::Asin && isz {
                return zero(s);
            }
            let is_one = x.is_one();
            if f == Func::Acos && is_one && !s {
                return zero(false);
            }
            if f == Func::Asin {
                if is_one {
                    return (if s { PI_2.neg() } else { PI_2 }).to_fpx(e);
                }
                // atan(x / sqrt((1 - x)(1 + x)))
                let d = ONE.sub(x).mul(ONE.add(x)).sqrt();
                atan_w(x.div(d)).to_fpx(e)
            } else {
                if is_one {
                    return PI_2.ldexp(1).to_fpx(e); // acos(-1) = pi
                }
                if isz {
                    return PI_2.to_fpx(e);
                }
                // 2 atan(sqrt((1 - x) / (1 + x)))
                let t = ONE.sub(x).div(ONE.add(x)).sqrt();
                atan_w(t).ldexp(1).to_fpx(e)
            }
        }
        Func::Atanh => {
            if above_one {
                return operr(e);
            }
            if isz {
                return zero(s);
            }
            if x.is_one() {
                return dz(e, s);
            }
            // atanh(x) = log1p(2x / (1 - x)) / 2
            let ax = x.abs();
            let v = log1p_w(ax.ldexp(1).div(ONE.sub(ax))).ldexp(-1);
            (if s { v.neg() } else { v }).to_fpx(e)
        }
        Func::Sinh => {
            if isi || isz {
                return if isi { inf(s) } else { zero(s) };
            }
            let ax = x.abs();
            if !ax.below(14) {
                return overflow(e, s);
            }
            let v = if ax.below(0) {
                // (E + E / (E + 1)) / 2, E = expm1(|x|)
                let em = expm1_small(ax);
                em.add(em.div(em.add(ONE))).ldexp(-1)
            } else {
                let ex = exp_w(ax);
                ex.sub(ONE.div(ex)).ldexp(-1)
            };
            (if s { v.neg() } else { v }).to_fpx(e)
        }
        Func::Cosh => {
            if isi {
                return inf(false);
            }
            if isz {
                return one(false);
            }
            let ax = x.abs();
            if !ax.below(14) {
                return overflow(e, false);
            }
            let ex = exp_w(ax);
            ex.add(ONE.div(ex)).ldexp(-1).to_fpx(e)
        }
        Func::Tanh => {
            if isi {
                return one(s);
            }
            if isz {
                return zero(s);
            }
            let ax = x.abs();
            let v = if !ax.below(5) {
                // 32 or above: 1 less under 2^-91, between the 64-bit
                // neighbours of 1 as 1 - 2^-128 is
                ONE_MINUS
            } else {
                // E / (E + 2), E = expm1(2|x|)
                let em = expm1_w(ax.ldexp(1));
                em.div(em.add(W::from_i64(2)))
            };
            (if s { v.neg() } else { v }).to_fpx(e)
        }
        Func::Etox | Func::Twotox | Func::Tentox => {
            if isi {
                return if s { zero(false) } else { inf(false) };
            }
            if isz {
                return one(false);
            }
            match (f, integral(x)) {
                // exact (or the overflow and underflow of the range)
                (Func::Twotox, Some(n)) => return pack(e, false, n as i32, TOP, 0),
                (Func::Tentox, Some(n)) if (0..=27).contains(&n) => {
                    // exact: no sticky bit, unlike to_fpx()
                    let p = pow10(n);
                    return pack(e, false, p.e, (p.m >> 64) as u64, p.m as u64);
                }
                _ => {}
            }
            // beyond the range: 2^14 (2^15 for twotox, whose results reach
            // 2^-16445)
            if !x.below(if f == Func::Twotox { 15 } else { 14 }) {
                return if s {
                    underflow(e, false)
                } else {
                    overflow(e, false)
                };
            }
            let v = match f {
                Func::Etox => exp_w(x),
                Func::Twotox => {
                    // 2^n * exp(frac * ln2)
                    let n = x.round_i64();
                    let fr = x.sub(W::from_i64(n));
                    exp_w(fr.mul(LN2)).ldexp(n as i32)
                }
                _ => exp_w(x.mul(LN10)),
            };
            v.to_fpx(e)
        }
        Func::Etoxm1 => {
            if isi {
                return if s { one(true) } else { inf(false) };
            }
            if isz {
                return zero(s);
            }
            if !x.below(14) {
                return if s {
                    ONE_MINUS.neg().to_fpx(e)
                } else {
                    overflow(e, false)
                };
            }
            expm1_w(x).to_fpx(e)
        }
        Func::Logn | Func::Log10 | Func::Log2 => {
            if isz {
                return dz(e, true);
            }
            if s {
                return operr(e);
            }
            if isi {
                return inf(false);
            }
            if x.is_one() {
                return zero(false);
            }
            match f {
                Func::Logn => log_w(x).to_fpx(e),
                Func::Log2 => {
                    if x.m == 1 << 127 {
                        // a power of 2: exact, though the C version calls
                        // an unnormalized one inexact
                        let c_exact = a.mantissa << 1 == 0
                            || (exp(a) == 0 && a.mantissa & (a.mantissa - 1) == 0);
                        let r = exact_int(e, i64::from(x.e));
                        if !c_exact {
                            e.flags |= INEX2;
                        }
                        return r;
                    }
                    let (big_e, lm) = log_parts(x);
                    W::from_i64(i64::from(big_e)).add(lm.mul(INV_LN2)).to_fpx(e)
                }
                _ => {
                    // exact for 10^0 .. 10^27
                    for n in 1..=27 {
                        let p = pow10(n);
                        if p.e == x.e && p.m == x.m {
                            return exact_int(e, n);
                        }
                    }
                    log_w(x).mul(INV_LN10).to_fpx(e)
                }
            }
        }
        Func::Lognp1 => {
            if isi {
                return if s { operr(e) } else { inf(false) };
            }
            if isz {
                return zero(s);
            }
            if s && x.is_one() {
                return dz(e, true);
            }
            if s && above_one {
                return operr(e);
            }
            log1p_w(x).to_fpx(e)
        }
    }
}
