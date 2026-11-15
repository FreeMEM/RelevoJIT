// SPDX-License-Identifier: MPL-2.0
//! MULU, MULS, DIVU and DIVS (the 16-bit operand forms of the 68000).
//!
//! Their time depends on the operands. Multiply: 38 + 2n cycles (M68000UM
//! table 8-4), n counting the source's ones (MULU) or the changes between
//! neighbouring bits of the source with a 0 appended below it (MULS).
//! Divide: the cycle counts of the 68000's shift-and-subtract loop, as
//! worked out from its documented best and worst cases and checked against
//! the SingleStepTests data. The flags of an overflowing divide (undefined
//! in the manual) are those of the data.

use super::{CCR_C, CCR_N, CCR_V, CCR_Z, EA_DATA, OpTable, SZ_L, SZ_W, reg9, sext};
use crate::cpu::{Cpu, Model, Res, VEC_ZERO_DIVIDE};

// ---- Multiply ----------------------------------------------------------------

fn op_mulu(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let s = c.ea_read(&mut e)?;
    let dn = reg9(op);
    let v = (c.r.d[dn] & 0xFFFF) * s;
    c.r.d[dn] = v;
    c.set_nz(v, SZ_L);
    c.cycles(34 + 2 * (s & 0xFFFF).count_ones());
    Ok(())
}

fn op_muls(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let s = c.ea_read(&mut e)?;
    let dn = reg9(op);
    let p = (sext(c.r.d[dn], SZ_W) as i32).wrapping_mul(sext(s, SZ_W) as i32);
    let v = p as u32;
    c.r.d[dn] = v;
    c.set_nz(v, SZ_L);
    c.cycles(34 + 2 * ((s ^ (s << 1)) & 0xFFFF).count_ones());
    Ok(())
}

// ---- Divide ------------------------------------------------------------------

/// Division by zero: the trap, after 38 cycles plus the operand's. The
/// flags (undefined in the manuals) are those of FS-UAE, the bench's oracle
/// (tools/oraculo): the 68000 and 68010 clear C; the 68020 and 68030 set V
/// and, for DIVU, N from the dividend's sign and Z the other way round, for
/// DIVS Z alone; the 68040 and 68060 clear N, Z, V and C for DIVU and C for
/// DIVS.
fn divide_by_zero(c: &mut Cpu, dividend: u32, sign: bool) -> Res {
    let nzvc = CCR_N | CCR_Z | CCR_V | CCR_C;
    let (mask, f) = match c.model {
        Model::M68020 | Model::M68030 => {
            let f = if sign {
                CCR_Z
            } else {
                CCR_V | if dividend >> 31 != 0 { CCR_N } else { CCR_Z }
            };
            (nzvc, f)
        }
        Model::M68040 | Model::M68060 if !sign => (nzvc, 0),
        _ => (CCR_C, 0),
    };
    c.set_ccr_bits(mask, f);
    c.cycles(4);
    c.exception(VEC_ZERO_DIVIDE, c.r.pc)
}

/// Overflow: V set, and in the data N set, Z and C clear. Dn stays.
fn divide_overflow(c: &mut Cpu) {
    c.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
}

/// Quotient in the low word, remainder in the high one.
fn divide_result(c: &mut Cpu, dn: usize, q: u32, r: u32) {
    c.r.d[dn] = (r & 0xFFFF) << 16 | (q & 0xFFFF);
    c.set_nz(q, SZ_W);
}

/// DIVU: 10 cycles on overflow; otherwise 76 plus up to 60 more, from the
/// quotient bits the loop works out (15 steps).
fn divu_cycles(mut dividend: u32, divisor: u32) -> u32 {
    let mut t = 38;
    let hd = divisor << 16;
    for _ in 0..15 {
        let top = dividend & 0x8000_0000 != 0;
        dividend <<= 1;
        if top {
            dividend = dividend.wrapping_sub(hd);
        } else {
            t += 2;
            if dividend >= hd {
                dividend -= hd;
                t -= 1;
            }
        }
    }
    2 * t
}

fn op_divu(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let s = c.ea_read(&mut e)?;
    let dn = reg9(op);
    let d = c.r.d[dn];
    if s == 0 {
        return divide_by_zero(c, d, false);
    }
    if d >> 16 >= s {
        c.cycles(10 - 4);
        divide_overflow(c);
        return Ok(());
    }
    c.cycles(divu_cycles(d, s) - 4);
    divide_result(c, dn, d / s, d % s);
    Ok(())
}

fn op_divs(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let s = c.ea_read(&mut e)?;
    let dn = reg9(op);
    let d = c.r.d[dn];
    if s == 0 {
        return divide_by_zero(c, d, true);
    }
    let dividend = d as i32;
    let divisor = sext(s, SZ_W) as i32;
    let ad = dividend.unsigned_abs();
    let as_ = divisor.unsigned_abs();

    let mut t: u32 = if dividend < 0 { 7 } else { 6 };
    if ad >> 16 >= as_ {
        c.cycles(2 * (t + 2) - 4);
        divide_overflow(c);
        return Ok(());
    }
    let aq = ad / as_;
    t += 55;
    if divisor >= 0 {
        if dividend < 0 {
            t += 1;
        } else {
            t -= 1;
        }
    }
    // one step more for each of the top 15 quotient bits that is 0
    for i in 1..=15 {
        if (aq >> i) & 1 == 0 {
            t += 1;
        }
    }
    c.cycles(2 * t - 4);
    if aq > 0x7FFF + u32::from((dividend < 0) != (divisor < 0)) {
        // the quotient does not fit a signed word
        divide_overflow(c);
        return Ok(());
    }
    let ar = ad % as_;
    let q = if (dividend < 0) != (divisor < 0) {
        aq.wrapping_neg()
    } else {
        aq
    };
    let r = if dividend < 0 { ar.wrapping_neg() } else { ar };
    divide_result(c, dn, q, r);
    Ok(())
}

pub fn fill(tb: &mut OpTable) {
    tb.fill(0xC0C0, 0x0E3F, EA_DATA, op_mulu);
    tb.fill(0xC1C0, 0x0E3F, EA_DATA, op_muls);
    tb.fill(0x80C0, 0x0E3F, EA_DATA, op_divu);
    tb.fill(0x81C0, 0x0E3F, EA_DATA, op_divs);
}
