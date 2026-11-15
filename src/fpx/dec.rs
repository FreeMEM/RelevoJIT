// SPDX-License-Identifier: MPL-2.0
//! Packed decimal conversions and the constant ROM of the 68881 family.
//! The decimal conversions are exact: they work on big integers, so the
//! result is the value correctly rounded with the FPCR's mode (the real
//! chips use a fixed internal precision and may differ in the last digit or
//! bit for some values).

use super::{BIAS, Env, Fpx, INEX1, INEX2, OPERR, QUIET, RM, RN, RZ, SNAN};
use super::{exp, is_nan, is_snan, make, pack, sign};

// ---- Big unsigned integers -------------------------------------------------

/// 10^4966 times a 64-bit mantissa is the largest value needed: about
/// 16 560 bits. They live on the stack (2 KB each).
const BIG_LIMBS: usize = 560;

#[derive(Clone)]
struct Big {
    /// Limbs used; d[n - 1] != 0 unless n == 0
    n: usize,
    d: [u32; BIG_LIMBS],
}

impl Big {
    fn from64(v: u64) -> Big {
        let mut a = Big {
            n: 2,
            d: [0; BIG_LIMBS],
        };
        a.d[0] = v as u32;
        a.d[1] = (v >> 32) as u32;
        a.trim();
        a
    }

    fn trim(&mut self) {
        while self.n > 0 && self.d[self.n - 1] == 0 {
            self.n -= 1;
        }
    }

    fn mul32(&mut self, m: u32) {
        let mut carry = 0u64;
        for i in 0..self.n {
            let p = u64::from(self.d[i]) * u64::from(m) + carry;
            self.d[i] = p as u32;
            carry = p >> 32;
        }
        if carry != 0 && self.n < BIG_LIMBS {
            self.d[self.n] = carry as u32;
            self.n += 1;
        }
    }

    fn mul_pow10(&mut self, mut n: u32) {
        while n >= 9 {
            self.mul32(1_000_000_000);
            n -= 9;
        }
        const P10: [u32; 9] = [
            1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
        ];
        if n != 0 {
            self.mul32(P10[n as usize]);
        }
    }

    fn shl(&mut self, bits: u32) {
        if self.n == 0 {
            return;
        }
        let w = (bits / 32) as usize;
        let b = bits % 32;
        let n = (self.n + w + 1).min(BIG_LIMBS);
        for i in (0..n).rev() {
            let src = i as isize - w as isize;
            let get = |j: isize| {
                if j >= 0 && (j as usize) < self.n {
                    self.d[j as usize]
                } else {
                    0
                }
            };
            let hi = get(src);
            let lo = get(src - 1);
            self.d[i] = if b != 0 { hi << b | lo >> (32 - b) } else { hi };
        }
        self.n = n;
        self.trim();
    }

    fn shr1(&mut self) {
        for i in 0..self.n {
            let next = if i + 1 < self.n { self.d[i + 1] } else { 0 };
            self.d[i] = self.d[i] >> 1 | next << 31;
        }
        self.trim();
    }

    fn bits(&self) -> u32 {
        if self.n == 0 {
            return 0;
        }
        32 * self.n as u32 - self.d[self.n - 1].leading_zeros()
    }

    fn cmp(&self, b: &Big) -> core::cmp::Ordering {
        if self.n != b.n {
            return self.n.cmp(&b.n);
        }
        for i in (0..self.n).rev() {
            if self.d[i] != b.d[i] {
                return self.d[i].cmp(&b.d[i]);
            }
        }
        core::cmp::Ordering::Equal
    }

    /// self -= b, with self >= b
    fn sub(&mut self, b: &Big) {
        let mut borrow = 0u64;
        for i in 0..self.n {
            let bi = if i < b.n { u64::from(b.d[i]) } else { 0 };
            let s = u64::from(self.d[i]).wrapping_sub(bi).wrapping_sub(borrow);
            self.d[i] = s as u32;
            borrow = (s >> 63) & 1;
        }
        self.trim();
    }

    fn bit(&self, i: u32) -> bool {
        let w = (i / 32) as usize;
        w < self.n && (self.d[w] >> (i % 32)) & 1 != 0
    }

    /// The quotient floor(self / b), known to be below 2^nbits (nbits <=
    /// 128); self is left with the remainder.
    fn div(&mut self, b: &Big, nbits: u32) -> u128 {
        let mut tt = b.clone();
        tt.shl(nbits - 1);
        let mut q: u128 = 0;
        for i in (0..nbits).rev() {
            if self.cmp(&tt) != core::cmp::Ordering::Less {
                self.sub(&tt);
                q |= 1 << i;
            }
            tt.shr1();
        }
        q
    }

    /// The top 128 bits (of `bits` bits), the rest as a sticky bit.
    fn top128(&self, bits: u32) -> (u64, u64) {
        let (mut hi, mut lo) = (0u64, 0u64);
        let mut sticky = false;
        for k in 0..bits {
            let i = bits - 1 - k; // bit of a, from the top
            if !self.bit(i) {
                continue;
            }
            if k < 64 {
                hi |= 1 << (63 - k);
            } else if k < 128 {
                lo |= 1 << (127 - k);
            } else {
                sticky = true;
            }
        }
        (hi, lo | u64::from(sticky))
    }
}

// ---- Packed decimal ----------------------------------------------------------

/// Packed decimal (MC68881UM 2.4.4) to extended: three longs, the first one
/// with the signs, the exponent and the integer digit. Rounds with the
/// environment and raises INEX1 if inexact.
pub fn from_packed(e: &mut Env, p: &[u32; 3]) -> Fpx {
    let sign = p[0] >> 31 != 0;
    if p[0] & 0x7FFF_0000 == 0x7FFF_0000 {
        let m = u64::from(p[1]) << 32 | u64::from(p[2]);
        return make(sign, 0x7FFF, m);
    }
    let mut exp10 =
        (((p[0] >> 24) & 0xF) * 100 + ((p[0] >> 20) & 0xF) * 10 + ((p[0] >> 16) & 0xF)) as i32;
    if p[0] & 0x4000_0000 != 0 {
        exp10 = -exp10;
    }
    let mut digits = u64::from(p[0] & 0xF);
    for i in 0..16 {
        let w = if i < 8 { p[1] } else { p[2] };
        digits = digits * 10 + u64::from((w >> (28 - 4 * (i % 8))) & 0xF);
    }
    if digits == 0 {
        return make(sign, 0, 0);
    }

    let n = exp10 - 16;
    let mut a = Big::from64(digits);
    let (hi, lo, exp) = if n >= 0 {
        a.mul_pow10(n as u32);
        let bits = a.bits();
        let (hi, lo) = a.top128(bits);
        (hi, lo, bits as i32 - 1)
    } else {
        let mut b = Big::from64(1);
        b.mul_pow10((-n) as u32);
        let s = 127 + b.bits() - a.bits();
        a.shl(s);
        let q = a.div(&b, 128);
        let lo = q as u64 | u64::from(a.n != 0);
        ((q >> 64) as u64, lo, 127 - s as i32)
    };
    let mut t = Env { flags: 0, ..*e };
    let r = pack(&mut t, sign, exp, hi, lo);
    e.flags |= (t.flags & !INEX2) | if t.flags & INEX2 != 0 { INEX1 } else { 0 };
    r
}

/// a = m * 2^p2 * 10^max(-t, 0), b = 10^max(t, 0) * 2^max(-p2, 0): a / b is
/// the value divided by 10^t.
fn scaled(m: u64, p2: i32, t: i32) -> (Big, Big) {
    let mut a = Big::from64(m);
    let mut b = Big::from64(1);
    if p2 > 0 {
        a.shl(p2 as u32);
    } else if p2 < 0 {
        b.shl((-p2) as u32);
    }
    if t < 0 {
        a.mul_pow10((-t) as u32);
    } else {
        b.mul_pow10(t as u32);
    }
    (a, b)
}

fn ge_pow10(m: u64, p2: i32, t: i32) -> bool {
    let (a, b) = scaled(m, p2, t);
    a.cmp(&b) != core::cmp::Ordering::Less
}

fn bcd3(v: u32) -> u32 {
    ((v / 100 % 10) << 8) | ((v / 10 % 10) << 4) | (v % 10)
}

/// Extended to packed decimal with the k-factor `k` (-64..63): k > 0
/// significant digits (17 at most; more is OPERR), k <= 0 digits after the
/// decimal point.
pub fn to_packed(e: &mut Env, mut x: Fpx, mut k: i32) -> [u32; 3] {
    let sign = sign(x);
    let mut p = [if sign { 0x8000_0000 } else { 0 }, 0, 0];
    if exp(x) == 0x7FFF {
        if is_nan(x) {
            if is_snan(x) {
                e.flags |= SNAN;
                x.mantissa |= QUIET;
            }
            p[1] = (x.mantissa >> 32) as u32;
            p[2] = x.mantissa as u32;
        }
        p[0] |= 0x7FFF_0000;
        return p;
    }
    if k > 17 {
        e.flags |= OPERR;
        k = 17;
    }
    if x.mantissa == 0 {
        return p;
    }
    // value = m * 2^p2, m normalized
    let s = x.mantissa.leading_zeros();
    let m = x.mantissa << s;
    let ex = exp(x);
    let e2 = (if ex == 0 { 1 } else { ex as i32 }) - BIAS - s as i32;
    let p2 = e2 - 63;
    // floor(e2 * log10(2)), near enough: the loops below settle it
    let mut e10 = ((i64::from(e2) * 78913) >> 18) as i32;
    while !ge_pow10(m, p2, e10) {
        e10 -= 1;
    }
    while ge_pow10(m, p2, e10 + 1) {
        e10 += 1;
    }
    let mut len = if k > 0 { k } else { e10 + 1 - k };
    len = len.clamp(1, 17);

    let t = e10 - len + 1;
    let (mut a, b) = scaled(m, p2, t);
    let mut q = a.div(&b, 60) as u64;
    let rest = a.n != 0;
    let mut h = a.clone();
    h.shl(1);
    let half = h.cmp(&b); // 2 * remainder against b
    let inc = match e.mode {
        RN => half.is_gt() || (half.is_eq() && q & 1 != 0),
        RZ => false,
        RM => rest && sign,
        _ => rest && !sign,
    };
    q += u64::from(inc);
    if rest {
        e.flags |= INEX2;
    }
    let limit = 10u64.pow(len as u32);
    if q >= limit {
        // 9.99 became 10.0
        e10 += 1;
        if k > 0 || len == 17 {
            q /= 10;
        } else {
            len += 1;
        }
    }
    for _ in len..17 {
        q *= 10;
    }
    // 17 digits: the integer digit, then 16
    let mut dg = [0u32; 17];
    for d in dg.iter_mut().rev() {
        *d = (q % 10) as u32;
        q /= 10;
    }
    let ae = e10.unsigned_abs();
    if ae > 999 {
        e.flags |= OPERR;
    }
    p[0] |= (if e10 < 0 { 0x4000_0000 } else { 0 })
        | bcd3(ae % 1000) << 16
        | (ae / 1000 % 10) << 12
        | dg[0];
    for i in 0..8 {
        p[1] |= dg[1 + i] << (28 - 4 * i);
        p[2] |= dg[9 + i] << (28 - 4 * i);
    }
    p
}

// ---- The constant ROM --------------------------------------------------------

/// Offset, exponent of the leading bit and 128 bits of mantissa (the last
/// bit set when the value goes on). Computed with exact big integer series.
/// As the real ROM, e and log10(2) round to nearest downwards: their low
/// half says "just above" the 64-bit value, so RN and RZ give it and RP the
/// next one.
const ROM: [(u32, i32, u64, u64); 22] = [
    (0x00, 1, 0xC90FDAA22168C234, 0xC4C6628B80DC1CD1), // pi
    (0x0B, -2, 0x9A209A84FBCFF798, 0x0000000000000001), // log10(2)
    (0x0C, 1, 0xADF85458A2BB4A9A, 0x0000000000000001), // e
    (0x0D, 0, 0xB8AA3B295C17F0BB, 0xBE87FED0691D3E89), // log2(e)
    (0x0E, -2, 0xDE5BD8A937287195, 0x355BAAAFAD33DC33), // log10(e)
    (0x0F, 0, 0, 0),                                   // 0.0
    (0x30, -1, 0xB17217F7D1CF79AB, 0xC9E3B39803F2F6AF), // ln(2)
    (0x31, 1, 0x935D8DDDAAA8AC16, 0xEA56D62B82D30A29), // ln(10)
    (0x32, 0, 0x8000000000000000, 0),                  // 10^0
    (0x33, 3, 0xA000000000000000, 0),
    (0x34, 6, 0xC800000000000000, 0),
    (0x35, 13, 0x9C40000000000000, 0),
    (0x36, 26, 0xBEBC200000000000, 0),
    (0x37, 53, 0x8E1BC9BF04000000, 0),
    (0x38, 106, 0x9DC5ADA82B70B59D, 0xF020000000000000),
    (0x39, 212, 0xC2781F49FFCFA6D5, 0x3CBF6B71C76B25FB),
    (0x3A, 425, 0x93BA47C980E98CDF, 0xC66F336C36B10137),
    (0x3B, 850, 0xAA7EEBFB9DF9DE8D, 0xDDBB901B98FEEAB7),
    (0x3C, 1700, 0xE319A0AEA60E91C6, 0xCC655C54BC5058F9),
    (0x3D, 3401, 0xC976758681750C17, 0x650D3D28F18B50CF),
    (0x3E, 6803, 0x9E8B3B5DC53D5DE4, 0xA74D28CE329ACE53),
    (0x3F, 13606, 0xC46052028A20979A, 0xC94C153F804A4A93), // 10^4096
];

/// FMOVECR: the constant at `offset` of the ROM, rounded with the
/// environment; None for an offset with no constant.
pub fn constant(e: &mut Env, offset: u32) -> Option<Fpx> {
    let &(_, exp, hi, lo) = ROM.iter().find(|r| r.0 == offset)?;
    if hi == 0 {
        return Some(make(false, 0, 0));
    }
    // The ROM's extended values count as exact: only a rounding to single
    // or double precision is inexact.
    let mut t = Env { flags: 0, ..*e };
    let r = pack(&mut t, false, exp, hi, lo);
    if e.prec < 64 {
        e.flags |= t.flags;
    }
    Some(r)
}
