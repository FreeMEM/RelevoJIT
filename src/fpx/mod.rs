// SPDX-License-Identifier: MPL-2.0
//! Extended precision floating point, as the MC68881/MC68882 and the FPU of
//! the 68040 and 68060 compute it. Shared by the engines.
//!
//! A value is an [`Fpx`] (`relevo_fpreg`): sign and 15-bit biased
//! exponent, then a 64-bit mantissa with an explicit integer bit
//! (MC68881UM 2.4). Inputs may be denormalized (exponent 0) or
//! unnormalized (integer bit 0, exponent not 0); every result is
//! normalized, or denormalized when it is too small.
//!
//! Each operation rounds to the precision and with the rounding mode of its
//! environment ([`Env`]) and ORs the exceptions it raises into `flags`, with
//! the bits of the FPSR exception byte ([`SNAN`]...). The precision only
//! rounds the mantissa (24 bits for single, 53 for double): the exponent
//! keeps the extended range, as in a floating-point data register.
//! Conversions to single and double for memory use the range of those
//! formats.
//!
//! Underflow ([`UNFL`]) is raised when the exact result is smaller than the
//! smallest normalized number (tininess before rounding), also when the
//! rounded result is exact; [`INEX2`] tells whether it was. The accrued
//! byte of the FPSR takes UNFL only together with INEX2 ([`accrued`]).
//!
//! NaNs: an operation on a signaling NaN raises [`SNAN`] and gives it back
//! quiet (bit 62 set). With two NaN operands the destination (the first
//! operand) wins. An invalid operation ([`OPERR`]) gives the default NaN,
//! all ones.
//!
//! The mantissas are computed exactly with 128-bit integers, then rounded
//! once. Written from the MC68881/MC68882 User's Manual and the IEEE 754
//! rules it follows (a translation of the C version's `fpx.c`).
#![allow(dead_code)]

mod dec;
mod trans;

pub use dec::{constant, from_packed, to_packed};
pub use trans::{Func, trans};

pub type Fpx = crate::cpu::FpReg;

/// The rounding environment: mode (`RN`...), precision in mantissa bits (64,
/// 53 or 24) and the exceptions raised so far.
#[derive(Clone, Copy, Default, Debug)]
pub struct Env {
    pub mode: u32,
    pub prec: u32,
    pub flags: u32,
}

/// Rounding modes: the RND field of the FPCR.
pub const RN: u32 = 0;
pub const RZ: u32 = 1;
pub const RM: u32 = 2;
pub const RP: u32 = 3;

/// Exception bits: the FPSR exception status byte (and the FPCR enable
/// byte), bits 15 to 8.
pub const BSUN: u32 = 0x8000;
pub const SNAN: u32 = 0x4000;
pub const OPERR: u32 = 0x2000;
pub const OVFL: u32 = 0x1000;
pub const UNFL: u32 = 0x0800;
pub const DZ: u32 = 0x0400;
pub const INEX2: u32 = 0x0200;
pub const INEX1: u32 = 0x0100;

/// FPSR condition code bits (31-24)
pub const CC_N: u32 = 0x0800_0000;
pub const CC_Z: u32 = 0x0400_0000;
pub const CC_I: u32 = 0x0200_0000;
pub const CC_NAN: u32 = 0x0100_0000;

const BIAS: i32 = 16383;
const EMIN: i32 = -16382;
const EMAX: i32 = 16383;
const TOP: u64 = 0x8000_0000_0000_0000;
const QUIET: u64 = 0x4000_0000_0000_0000;

// ---- Classes -----------------------------------------------------------

#[inline]
pub fn sign(a: Fpx) -> bool {
    a.sign_exponent & 0x8000 != 0
}

#[inline]
pub fn exp(a: Fpx) -> u32 {
    u32::from(a.sign_exponent & 0x7FFF)
}

#[inline]
pub fn is_nan(a: Fpx) -> bool {
    exp(a) == 0x7FFF && a.mantissa << 1 != 0
}

#[inline]
pub fn is_snan(a: Fpx) -> bool {
    is_nan(a) && a.mantissa & QUIET == 0
}

#[inline]
pub fn is_inf(a: Fpx) -> bool {
    exp(a) == 0x7FFF && a.mantissa << 1 == 0
}

#[inline]
pub fn is_zero(a: Fpx) -> bool {
    exp(a) != 0x7FFF && a.mantissa == 0
}

#[inline]
pub fn make(sign: bool, exp: u32, mantissa: u64) -> Fpx {
    Fpx {
        mantissa,
        sign_exponent: (if sign { 0x8000 } else { 0 }) | (exp & 0x7FFF) as u16,
    }
}

/// All ones: what an invalid operation gives, and the reset value of the
/// data registers.
#[inline]
pub fn default_nan() -> Fpx {
    make(false, 0x7FFF, !0)
}

/// The condition code byte of the FPSR for a result (N is the sign, also
/// of zeros, infinities and NaNs).
pub fn cc(a: Fpx) -> u32 {
    let mut cc = if sign(a) { CC_N } else { 0 };
    if is_nan(a) {
        cc |= CC_NAN;
    } else if is_inf(a) {
        cc |= CC_I;
    } else if a.mantissa == 0 {
        cc |= CC_Z;
    }
    cc
}

/// The accrued exception byte (FPSR bits 7-3) that the exception bits
/// `f` add: IOP from SNAN/OPERR (and BSUN), OVFL, UNFL with INEX2, DZ,
/// INEX from INEX1/INEX2/OVFL.
pub fn accrued(f: u32) -> u32 {
    let mut a = 0;
    if f & (BSUN | SNAN | OPERR) != 0 {
        a |= 0x80;
    }
    if f & OVFL != 0 {
        a |= 0x40;
    }
    if f & UNFL != 0 && f & INEX2 != 0 {
        a |= 0x20;
    }
    if f & DZ != 0 {
        a |= 0x10;
    }
    if f & (INEX1 | INEX2 | OVFL) != 0 {
        a |= 0x08;
    }
    a
}

// ---- Helpers -----------------------------------------------------------

#[inline]
fn hl(hi: u64, lo: u64) -> u128 {
    u128::from(hi) << 64 | u128::from(lo)
}

#[inline]
fn split(v: u128) -> (u64, u64) {
    ((v >> 64) as u64, v as u64)
}

/// Shift right; the bits shifted out are ORed into bit 0 (sticky).
pub(crate) fn shr_sticky(a: u128, n: u32) -> u128 {
    if n == 0 {
        a
    } else if n >= 128 {
        u128::from(a != 0)
    } else {
        a >> n | u128::from(a << (128 - n) != 0)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Class {
    Zero,
    Norm,
    Inf,
    Nan,
}

#[derive(Clone, Copy)]
struct Unp {
    cls: Class,
    sign: bool,
    /// Of bit 63 of m
    exp: i32,
    /// Norm: bit 63 set
    m: u64,
}

fn unpack(a: Fpx) -> Unp {
    let e = exp(a);
    let mut u = Unp {
        cls: Class::Norm,
        sign: sign(a),
        exp: 0,
        m: a.mantissa,
    };
    if e == 0x7FFF {
        u.cls = if a.mantissa << 1 != 0 {
            Class::Nan
        } else {
            Class::Inf
        };
        return u;
    }
    if a.mantissa == 0 {
        u.cls = Class::Zero;
        return u;
    }
    let s = u.m.leading_zeros();
    u.m <<= s;
    // A denormal's exponent field 0 means the same scale as 1.
    u.exp = (if e == 0 { 1 } else { e as i32 }) - BIAS - s as i32;
    u
}

fn zero(sign: bool) -> Fpx {
    make(sign, 0, 0)
}

fn inf(sign: bool) -> Fpx {
    make(sign, 0x7FFF, 0)
}

fn operr(e: &mut Env) -> Fpx {
    e.flags |= OPERR;
    default_nan()
}

fn quiet(mut a: Fpx) -> Fpx {
    a.mantissa |= QUIET;
    a
}

/// The NaN result of an operation with at least one NaN operand: the
/// destination `a` if it is one, else `b`.
fn nan2(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    if is_snan(a) || is_snan(b) {
        e.flags |= SNAN;
    }
    quiet(if is_nan(a) { a } else { b })
}

fn nan1(e: &mut Env, a: Fpx) -> Fpx {
    if is_snan(a) {
        e.flags |= SNAN;
    }
    quiet(a)
}

// ---- Rounding ----------------------------------------------------------

/// value = (hi * 2^64 + lo) * 2^(exp - 127): rounds to `prec` bits with the
/// exponent range [emin, emax] (of the leading bit), denormalizing below
/// emin. Gives back the exponent and a 64-bit mantissa (leading bit at 63
/// unless denormalized at emin, or 0 for a zero). Overflow gives an
/// infinity (exponent emax + 1) or the largest number, as the mode says.
#[allow(clippy::too_many_arguments)]
pub fn round_raw(
    e: &mut Env,
    sign: bool,
    exp: i32,
    hi: u64,
    lo: u64,
    prec: u32,
    emin: i32,
    emax: i32,
) -> (i32, u64) {
    let mut v = hl(hi, lo);
    if v == 0 {
        return (exp, 0);
    }
    // Normalize: leading bit at 127.
    let s = v.leading_zeros();
    v <<= s;
    let mut exp = exp - s as i32;
    let tiny = exp < emin;
    if tiny {
        let d = i64::from(emin) - i64::from(exp);
        v = shr_sticky(v, d.min(200) as u32);
        exp = emin;
    }
    let (hi, lo) = split(v);
    let drop = 64 - prec; // bits of hi below the precision
    let (rbit, sticky) = if drop == 0 {
        (lo >> 63 != 0, lo << 1 != 0)
    } else {
        (
            (hi >> (drop - 1)) & 1 != 0,
            hi & ((1u64 << (drop - 1)) - 1) != 0 || lo != 0,
        )
    };
    let keep = if drop != 0 { !((1u64 << drop) - 1) } else { !0 };
    let mut r = hi & keep;
    let inexact = rbit || sticky;
    let inc = match e.mode {
        RN => rbit && (sticky || (r >> drop) & 1 != 0),
        RZ => false,
        RM => inexact && sign,
        _ => inexact && !sign,
    };
    if inc {
        let one = 1u64 << drop;
        r = r.wrapping_add(one);
        if r < one {
            // carried out of bit 63
            r = TOP;
            exp += 1;
        }
    }
    if tiny {
        e.flags |= UNFL;
    }
    if inexact {
        e.flags |= INEX2;
    }
    if exp > emax {
        e.flags |= OVFL | INEX2;
        let to_inf = e.mode == RN || (e.mode == RM && sign) || (e.mode == RP && !sign);
        if to_inf {
            return (emax + 1, 0);
        }
        exp = emax;
        r = keep;
    }
    (exp, r)
}

/// The same as [`round_raw`], in the extended range, packed.
pub fn pack(e: &mut Env, sign: bool, exp: i32, hi: u64, lo: u64) -> Fpx {
    let (exp, hi) = round_raw(e, sign, exp, hi, lo, e.prec, EMIN, EMAX);
    if exp > EMAX {
        return inf(sign);
    }
    if hi == 0 {
        return zero(sign);
    }
    if hi >> 63 == 0 {
        return make(sign, 0, hi); // denormalized
    }
    make(sign, (exp + BIAS) as u32, hi)
}

fn pack_unp(e: &mut Env, u: Unp) -> Fpx {
    pack(e, u.sign, u.exp, u.m, 0)
}

// ---- Operations --------------------------------------------------------

fn add_unp(e: &mut Env, mut a: Unp, mut b: Unp) -> Fpx {
    if a.cls == Class::Zero && b.cls == Class::Zero {
        let s = if a.sign == b.sign {
            a.sign
        } else {
            e.mode == RM
        };
        return zero(s);
    }
    if a.cls == Class::Zero {
        return pack_unp(e, b);
    }
    if b.cls == Class::Zero {
        return pack_unp(e, a);
    }
    if a.exp < b.exp || (a.exp == b.exp && a.m < b.m) {
        core::mem::swap(&mut a, &mut b);
    }
    let d = i64::from(a.exp) - i64::from(b.exp);
    let x = hl(a.m, 0);
    let y = shr_sticky(hl(b.m, 0), d.min(200) as u32);
    let mut exp = a.exp;
    if a.sign == b.sign {
        let (mut s, carry) = x.overflowing_add(y);
        if carry {
            s = shr_sticky(s, 1) | 1 << 127;
            exp += 1;
        }
        let (h, l) = split(s);
        return pack(e, a.sign, exp, h, l);
    }
    let s = x - y;
    if s == 0 {
        return zero(e.mode == RM);
    }
    let (h, l) = split(s);
    pack(e, a.sign, exp, h, l)
}

fn addsub(e: &mut Env, a: Fpx, b: Fpx, sub: bool) -> Fpx {
    if is_nan(a) || is_nan(b) {
        return nan2(e, a, b);
    }
    let ua = unpack(a);
    let mut ub = unpack(b);
    ub.sign ^= sub;
    if ua.cls == Class::Inf {
        if ub.cls == Class::Inf && ua.sign != ub.sign {
            return operr(e);
        }
        return inf(ua.sign);
    }
    if ub.cls == Class::Inf {
        return inf(ub.sign);
    }
    add_unp(e, ua, ub)
}

/// `a + b`; `a` is the destination operand (as in every operation here).
pub fn add(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    addsub(e, a, b, false)
}

pub fn sub(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    addsub(e, a, b, true)
}

pub fn mul(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    if is_nan(a) || is_nan(b) {
        return nan2(e, a, b);
    }
    let ua = unpack(a);
    let ub = unpack(b);
    let s = ua.sign != ub.sign;
    if ua.cls == Class::Inf || ub.cls == Class::Inf {
        if ua.cls == Class::Zero || ub.cls == Class::Zero {
            return operr(e);
        }
        return inf(s);
    }
    if ua.cls == Class::Zero || ub.cls == Class::Zero {
        return zero(s);
    }
    let (h, l) = split(u128::from(ua.m) * u128::from(ub.m));
    pack(e, s, ua.exp + ub.exp + 1, h, l)
}

pub fn div(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    if is_nan(a) || is_nan(b) {
        return nan2(e, a, b);
    }
    let ua = unpack(a);
    let ub = unpack(b);
    let s = ua.sign != ub.sign;
    if ua.cls == Class::Inf {
        return if ub.cls == Class::Inf {
            operr(e)
        } else {
            inf(s)
        };
    }
    if ub.cls == Class::Inf {
        return zero(s);
    }
    if ub.cls == Class::Zero {
        if ua.cls == Class::Zero {
            return operr(e);
        }
        e.flags |= DZ;
        return inf(s);
    }
    if ua.cls == Class::Zero {
        return zero(s);
    }
    // 128 bits of x / y (both with bit 63 set): floor(x / y * 2^127), with
    // a sticky bit for the remainder. x * 2^127 / y in two 64-bit steps.
    let x = hl(ua.m, 0) >> 1; // x * 2^63
    let y = u128::from(ub.m);
    let q1 = x / y;
    let r1 = x % y;
    let r = r1 << 64;
    let q0 = r / y;
    let rest = r % y != 0;
    let q = (q1 << 64 | q0) | u128::from(rest);
    let (h, l) = split(q);
    pack(e, s, ua.exp - ub.exp, h, l)
}

/// floor(sqrt(v)) and whether it is exact, for a 128-bit v.
pub(crate) fn isqrt128(v: u128) -> (u64, u128) {
    // Digit by digit: root = floor(sqrt(v)), rem = v - root^2.
    let mut rem: u128 = 0;
    let mut root: u64 = 0;
    for i in (0..64).rev() {
        let two = (v >> (2 * i)) & 3;
        rem = rem << 2 | two;
        let trial = u128::from(root) << 2 | 1;
        root <<= 1;
        if rem >= trial {
            rem -= trial;
            root |= 1;
        }
    }
    (root, rem)
}

pub fn sqrt(e: &mut Env, a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    let u = unpack(a);
    if u.cls == Class::Zero {
        return zero(u.sign);
    }
    if u.sign {
        return operr(e);
    }
    if u.cls == Class::Inf {
        return inf(false);
    }
    // value = m * 2^(exp - 63) = R * 2^k with k even
    let (rad, k) = if u.exp & 1 != 0 {
        (hl(u.m, 0), u.exp - 127)
    } else {
        (hl(u.m >> 1, u.m << 63), u.exp - 126)
    };
    let (root, rem) = isqrt128(rad);
    // The next bit is 1 when rem > root; then there is more after it.
    let lo = if rem > u128::from(root) {
        0x8000_0000_0000_0001
    } else {
        u64::from(rem != 0)
    };
    pack(e, false, k / 2 + 63, root, lo)
}

/// FMOVE into a register: normalizes and rounds to the precision.
pub fn round(e: &mut Env, a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    let u = unpack(a);
    match u.cls {
        Class::Inf => inf(u.sign),
        Class::Zero => zero(u.sign),
        _ => pack_unp(e, u),
    }
}

pub fn abs(e: &mut Env, mut a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    a.sign_exponent &= 0x7FFF;
    round(e, a)
}

pub fn neg(e: &mut Env, mut a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    a.sign_exponent ^= 0x8000;
    round(e, a)
}

/// FINT (with the environment's mode) and FINTRZ (`mode` RZ).
pub fn int(e: &mut Env, a: Fpx, mode: u32) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    let u = unpack(a);
    match u.cls {
        Class::Inf => return inf(u.sign),
        Class::Zero => return zero(u.sign),
        _ => {}
    }
    if u.exp >= 63 {
        return pack_unp(e, u);
    }
    // integer part and the bits below it
    let (mut n, rbit, sticky) = if u.exp < 0 {
        (0u64, u.exp == -1, u.exp < -1 || u.m << 1 != 0)
    } else {
        let fb = 63 - u.exp as u32; // fraction bits, 1..63
        (
            u.m >> fb,
            (u.m >> (fb - 1)) & 1 != 0,
            u.m & ((1u64 << (fb - 1)) - 1) != 0,
        )
    };
    let inexact = rbit || sticky;
    let inc = match mode {
        RN => rbit && (sticky || n & 1 != 0),
        RZ => false,
        RM => inexact && u.sign,
        _ => inexact && !u.sign,
    };
    n += u64::from(inc);
    if inexact {
        e.flags |= INEX2;
    }
    if n == 0 {
        return zero(u.sign);
    }
    pack(e, u.sign, 63, n, 0)
}

pub fn getexp(e: &mut Env, a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    let u = unpack(a);
    match u.cls {
        Class::Inf => operr(e),
        Class::Zero => zero(u.sign),
        _ => from_int(e, u.exp),
    }
}

pub fn getman(e: &mut Env, a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    let mut u = unpack(a);
    match u.cls {
        Class::Inf => operr(e),
        Class::Zero => zero(u.sign),
        _ => {
            u.exp = 0;
            pack_unp(e, u)
        }
    }
}

/// a * 2^trunc(b)
pub fn scale(e: &mut Env, a: Fpx, b: Fpx) -> Fpx {
    if is_nan(a) || is_nan(b) {
        return nan2(e, a, b);
    }
    let ua = unpack(a);
    let ub = unpack(b);
    if ub.cls == Class::Inf {
        return operr(e);
    }
    if ua.cls == Class::Inf {
        return inf(ua.sign);
    }
    if ua.cls == Class::Zero {
        return zero(ua.sign);
    }
    // the scale, truncated toward zero, limited to well past the range
    let mut n: i32 = 0;
    if ub.cls == Class::Norm && ub.exp >= 0 {
        n = if ub.exp > 16 {
            0x20000
        } else {
            (ub.m >> (63 - ub.exp)) as i32
        };
    }
    if ub.sign {
        n = -n;
    }
    pack(e, ua.sign, ua.exp + n, ua.m, 0)
}

/// FMOD (`ieee` false: quotient truncated) and FREM (`ieee` true: quotient
/// rounded to nearest even). Also gives the FPSR quotient byte: the sign
/// of the quotient and its 7 low bits.
pub fn rem(e: &mut Env, a: Fpx, b: Fpx, ieee: bool) -> (Fpx, u8) {
    let ua = unpack(a);
    let ub = unpack(b);
    let qsign: u8 = if ua.sign != ub.sign { 0x80 } else { 0 };
    if is_nan(a) || is_nan(b) {
        return (nan2(e, a, b), 0);
    }
    if ua.cls == Class::Inf || ub.cls == Class::Zero {
        return (operr(e), 0);
    }
    if ua.cls == Class::Zero {
        return (zero(ua.sign), qsign);
    }
    if ub.cls == Class::Inf {
        return (round(e, a), qsign);
    }
    // r = |a| - q |b| with q = trunc(|a| / |b|), bit by bit
    let mut r = ua.m;
    let y = ub.m;
    let mut q: u64 = 0;
    let d = i64::from(ua.exp) - i64::from(ub.exp);
    let mut rexp = ua.exp; // exponent of bit 63 of r
    if d >= 0 {
        let mut carry = false;
        let mut i = 0;
        while i <= d {
            q <<= 1;
            if carry || r >= y {
                r = r.wrapping_sub(y);
                q |= 1;
            }
            if i < d {
                carry = r >> 63 != 0;
                r <<= 1;
            }
            i += 1;
        }
        rexp = ub.exp;
    }
    let mut rsign = ua.sign;
    if ieee {
        // round the quotient to nearest even: compare 2r with y
        let c: i32 = if d < -1 || r == 0 {
            -1
        } else if d == -1 {
            // |a| = r * 2^(rexp-63) with rexp = ub.exp - 1: 2r vs y
            (r.cmp(&y)) as i32
        } else if r >> 63 != 0 {
            1
        } else {
            ((r << 1).cmp(&y)) as i32
        };
        if c > 0 || (c == 0 && q & 1 != 0) {
            // r' = y - r, in the scale of y
            if d == -1 {
                // a has exponent ub.exp - 1: y - r/2
                let s = hl(y, 0) - shr_sticky(hl(r, 0), 1);
                q = q.wrapping_add(1);
                let (h, l) = split(s);
                return (pack(e, !rsign, ub.exp, h, l), qsign | (q & 0x7F) as u8);
            }
            r = y - r;
            q = q.wrapping_add(1);
            rsign = !rsign;
        }
    }
    let quotient = qsign | (q & 0x7F) as u8;
    if r == 0 {
        return (zero(ua.sign), quotient);
    }
    (pack(e, rsign, rexp, r, 0), quotient)
}

/// FCMP: the condition codes of a - b (N, Z, NAN; I never).
pub fn cmp(e: &mut Env, a: Fpx, b: Fpx) -> u32 {
    if is_nan(a) || is_nan(b) {
        nan2(e, a, b);
        return CC_NAN;
    }
    let ua = unpack(a);
    let ub = unpack(b);
    if ua.cls == Class::Inf && ub.cls == Class::Inf && ua.sign == ub.sign {
        return CC_Z | if ua.sign { CC_N } else { 0 };
    }
    // order: -inf < negatives < zeros < positives < +inf
    let c: i32 = if ua.cls == Class::Zero && ub.cls == Class::Zero {
        0
    } else if ua.cls == Class::Zero || ub.cls == Class::Zero {
        if ua.cls == Class::Zero {
            if ub.sign { 1 } else { -1 }
        } else if ua.sign {
            -1
        } else {
            1
        }
    } else if ua.sign != ub.sign {
        if ua.sign { -1 } else { 1 }
    } else {
        let mag: i32 = if ua.cls == Class::Inf || ub.cls == Class::Inf {
            if ua.cls == Class::Inf { 1 } else { -1 }
        } else if ua.exp != ub.exp {
            if ua.exp > ub.exp { 1 } else { -1 }
        } else {
            ua.m.cmp(&ub.m) as i32
        };
        if ua.sign { -mag } else { mag }
    };
    if c < 0 {
        return CC_N;
    }
    if c > 0 {
        return 0;
    }
    // equal: the sign of the zero a - b would give
    let neg = if ua.cls == Class::Zero {
        (ua.sign && !ub.sign) || (ua.sign == ub.sign && e.mode == RM)
    } else {
        e.mode == RM
    };
    CC_Z | if neg { CC_N } else { 0 }
}

// ---- Conversions -------------------------------------------------------

/// Integers to extended: exact, then rounded to the environment's
/// precision.
pub fn from_int(e: &mut Env, v: i32) -> Fpx {
    if v == 0 {
        return zero(false);
    }
    pack(e, v < 0, 63, u64::from(v.unsigned_abs()), 0)
}

/// Single to extended, exact (a signaling NaN stays signaling: the
/// operation that uses it raises SNAN).
pub fn from_single(v: u32) -> Fpx {
    let s = v >> 31 != 0;
    let ex = (v >> 23) & 0xFF;
    let f = u64::from(v & 0x7F_FFFF);
    if ex == 0xFF {
        return make(s, 0x7FFF, if f != 0 { TOP | f << 40 } else { 0 });
    }
    if ex == 0 {
        if f == 0 {
            return zero(s);
        }
        let sh = f.leading_zeros() as i32;
        // value = f * 2^-149, leading bit at 63 - sh
        return make(s, (63 - sh - 149 + BIAS) as u32, f << sh);
    }
    make(s, ex + BIAS as u32 - 127, TOP | f << 40)
}

pub fn from_double(v: u64) -> Fpx {
    let s = v >> 63 != 0;
    let ex = ((v >> 52) & 0x7FF) as u32;
    let f = v & 0xF_FFFF_FFFF_FFFF;
    if ex == 0x7FF {
        return make(s, 0x7FFF, if f != 0 { TOP | f << 11 } else { 0 });
    }
    if ex == 0 {
        if f == 0 {
            return zero(s);
        }
        let sh = f.leading_zeros() as i32;
        return make(s, (63 - sh - 1074 + BIAS) as u32, f << sh);
    }
    make(s, ex + BIAS as u32 - 1023, TOP | f << 11)
}

/// To an integer of `size` bytes (1, 2, 4), rounded with the mode. Out of
/// range, an infinity or a NaN: OPERR and the largest integer of the
/// operand's sign.
pub fn to_int(e: &mut Env, a: Fpx, size: u32) -> i32 {
    let max: i64 = 1 << (8 * size - 1); // |min|; max is one less
    let limit = |neg: bool| (if neg { -max } else { max - 1 }) as i32;
    if is_nan(a) {
        nan1(e, a);
        e.flags |= OPERR;
        return limit(sign(a));
    }
    let u = unpack(a);
    if u.cls == Class::Inf {
        e.flags |= OPERR;
        return limit(u.sign);
    }
    if u.cls == Class::Zero {
        return 0;
    }
    let mut t = Env {
        mode: e.mode,
        prec: 64,
        flags: 0,
    };
    let r = int(&mut t, a, e.mode);
    let ur = unpack(r);
    if ur.cls == Class::Zero {
        e.flags |= t.flags & INEX2;
        return 0;
    }
    let mag = if ur.exp >= 63 {
        !0u64
    } else {
        ur.m >> (63 - ur.exp)
    };
    if ur.exp >= 40 || mag as i64 > (if ur.sign { max } else { max - 1 }) {
        e.flags |= OPERR;
        return limit(ur.sign);
    }
    e.flags |= t.flags & INEX2;
    (if ur.sign { -(mag as i64) } else { mag as i64 }) as i32
}

/// To single (fbits 23, emin -126, emax 127) or double (52, -1022, 1023).
fn to_ieee(e: &mut Env, a: Fpx, fbits: u32, emin: i32, emax: i32) -> u64 {
    let ebits = if fbits == 23 { 8 } else { 11 };
    let emask = (1u64 << ebits) - 1;
    let sbit = u64::from(sign(a)) << (fbits + ebits);
    if is_nan(a) {
        let q = nan1(e, a);
        return sbit | emask << fbits | (q.mantissa << 1) >> (64 - fbits);
    }
    let u = unpack(a);
    match u.cls {
        Class::Inf => return sbit | emask << fbits,
        Class::Zero => return sbit,
        _ => {}
    }
    let (exp, m) = round_raw(e, u.sign, u.exp, u.m, 0, fbits + 1, emin, emax);
    if exp > emax {
        return sbit | emask << fbits;
    }
    if m == 0 {
        return sbit;
    }
    let f = (m << 1) >> (64 - fbits);
    if m >> 63 == 0 {
        // denormalized
        return sbit | m >> (63 - fbits);
    }
    sbit | ((exp - emin + 1) as u64) << fbits | f
}

pub fn to_single(e: &mut Env, a: Fpx) -> u32 {
    to_ieee(e, a, 23, -126, 127) as u32
}

pub fn to_double(e: &mut Env, a: Fpx) -> u64 {
    to_ieee(e, a, 52, -1022, 1023)
}

/// FMOVE.X out: the register as it is, a signaling NaN made quiet.
pub fn to_extended(e: &mut Env, a: Fpx) -> Fpx {
    if is_nan(a) {
        return nan1(e, a);
    }
    a
}

#[cfg(test)]
mod tests {
    use super::*;

    fn x(se: u16, m: u64) -> Fpx {
        Fpx {
            mantissa: m,
            sign_exponent: se,
        }
    }

    fn env(mode: u32, prec: u32) -> Env {
        Env {
            mode,
            prec,
            flags: 0,
        }
    }

    const ONE: Fpx = Fpx {
        mantissa: TOP,
        sign_exponent: 0x3FFF,
    };
    const THREE: Fpx = Fpx {
        mantissa: 0xC000_0000_0000_0000,
        sign_exponent: 0x4000,
    };

    #[test]
    fn basic() {
        let mut e = env(RN, 64);
        assert_eq!(div(&mut e, ONE, THREE), x(0x3FFD, 0xAAAA_AAAA_AAAA_AAAB));
        assert_eq!(e.flags, INEX2);
        let mut e = env(RZ, 24);
        assert_eq!(div(&mut e, ONE, THREE), x(0x3FFD, 0xAAAA_AA00_0000_0000));
        let mut e = env(RN, 64);
        assert_eq!(
            sqrt(&mut e, x(0x4000, TOP)),
            x(0x3FFF, 0xB504_F333_F9DE_6484)
        );
        assert!(is_nan(sqrt(&mut e, x(0xBFFF, TOP))));
        assert_eq!(e.flags & OPERR, OPERR);
        let (r, q) = rem(
            &mut env(RN, 64),
            x(0x4002, 0xA000_0000_0000_0000),
            THREE,
            true,
        );
        assert_eq!((r, q), (x(0x3FFF, TOP), 3)); // 10 rem 3 = 1
        assert_eq!(
            to_int(&mut env(RN, 64), x(0x4000, 0xA000_0000_0000_0000), 4),
            2
        ); // 2.5
        assert_eq!(from_double(0x3FF0_0000_0000_0000), ONE);
    }

    #[test]
    fn packed_and_constants() {
        let mut e = env(RN, 64);
        // 1.0E0: integer digit 1
        assert_eq!(from_packed(&mut e, &[0x0000_0001, 0, 0]), ONE);
        assert_eq!(to_packed(&mut e, ONE, 17), [0x0000_0001, 0, 0]);
        // pi, ln 2
        assert_eq!(constant(&mut e, 0), Some(x(0x4000, 0xC90F_DAA2_2168_C235)));
        assert_eq!(
            constant(&mut e, 0x30),
            Some(x(0x3FFE, 0xB172_17F7_D1CF_79AC))
        );
        assert_eq!(constant(&mut e, 1), None);
        assert_eq!(e.flags, 0);
    }

    #[test]
    fn transcendental() {
        let mut e = env(RN, 64);
        // correctly rounded: sin(1) = 0.D76AA47848677020C6E9E..., e =
        // 2.B7E151628AED2A6ABF71..., atan(1) = pi/4 = 0.C90FDAA22168C234C4...
        assert_eq!(
            trans(&mut e, Func::Sin, ONE),
            x(0x3FFE, 0xD76A_A478_4867_7021)
        );
        assert_eq!(
            trans(&mut e, Func::Etox, ONE),
            x(0x4000, 0xADF8_5458_A2BB_4A9B)
        );
        assert_eq!(
            trans(&mut e, Func::Atan, ONE),
            x(0x3FFE, 0xC90F_DAA2_2168_C235)
        );
        assert_eq!(e.flags, INEX2);
        // the rounding mode acts on the true value
        let mut e = env(RZ, 64);
        assert_eq!(
            trans(&mut e, Func::Etox, ONE),
            x(0x4000, 0xADF8_5458_A2BB_4A9A)
        );
        // exact results
        let mut e = env(RN, 64);
        assert_eq!(trans(&mut e, Func::Twotox, THREE), x(0x4002, TOP));
        assert_eq!(
            trans(&mut e, Func::Tentox, THREE),
            x(0x4008, 0xFA00_0000_0000_0000)
        );
        assert_eq!(
            trans(&mut e, Func::Log2, x(0x4005, TOP)),
            x(0x4001, 0xC000_0000_0000_0000)
        );
        assert_eq!(trans(&mut e, Func::Acos, ONE), x(0, 0));
        assert_eq!(e.flags, 0);
        // huge arguments: sin(2^16000) and cos, reduced exactly
        let big = x(0x3FFF + 16000, TOP);
        let s = trans(&mut e, Func::Sin, big);
        let c = trans(&mut e, Func::Cos, big);
        let s2 = mul(&mut e, s, s);
        let c2 = mul(&mut e, c, c);
        let one = add(&mut e, s2, c2);
        assert_eq!(one.sign_exponent & 0x7FFE, 0x3FFE);
    }
}
