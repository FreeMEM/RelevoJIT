// SPDX-License-Identifier: MPL-2.0
//! Shifts and rotates (ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR), bit
//! operations (BTST, BCHG, BCLR, BSET) and BCD arithmetic (ABCD, SBCD,
//! NBCD). Multiply and divide are in muldiv.rs.
//!
//! Cycles follow the M68000UM timing tables; the flags the manuals leave
//! undefined (N and V of the BCD instructions) are those of the
//! SingleStepTests/m68000 data.

use super::{
    CCR_C, CCR_N, CCR_V, CCR_X, CCR_Z, EA_DATA, EA_DATA_ALT, EA_MEM_ALT, EAB_IMM, EaKind, OpTable,
    SZ_B, SZ_L, SZ_W, reg0, reg9, size_field, sz_mask,
};
use crate::cpu::{ACC_DATA, Cpu, Res};

// ---- Shifts and rotates --------------------------------------------------------

const SH_AS: u32 = 0;
const SH_LS: u32 = 1;
const SH_ROX: u32 = 2;
const SH_RO: u32 = 3;

/// Shifts or rotates the `size`-byte value v by `count` (0-63) and sets the
/// flags (M68000PRM chapter 4). Returns the result.
fn shift(c: &mut Cpu, ty: u32, left: bool, v: u32, count: u32, size: u32) -> u32 {
    let bits = 8 * size;
    let mask = u64::from(sz_mask(size));
    let x = u64::from(v) & mask;
    let mut res = x;
    let mut carry = false;
    let mut overflow = false;
    let mut xflag = u32::from(c.r.sr) & CCR_X != 0;
    let mut set_x = count != 0; // X is untouched by a count of zero

    match ty {
        SH_AS | SH_LS => {
            if count != 0 {
                if left {
                    if count <= bits {
                        carry = (x >> (bits - count)) & 1 != 0;
                    }
                    res = if count < bits { (x << count) & mask } else { 0 };
                    if ty == SH_AS {
                        // V: the sign bit changed at some point, that is,
                        // the top count + 1 bits were not all equal.
                        if count >= bits {
                            overflow = x != 0;
                        } else {
                            let top = mask & !(mask >> (count + 1));
                            overflow = x & top != 0 && x & top != top;
                        }
                    }
                } else if ty == SH_LS {
                    if count <= bits {
                        carry = (x >> (count - 1)) & 1 != 0;
                    }
                    res = if count < bits { x >> count } else { 0 };
                } else {
                    let neg = (x >> (bits - 1)) & 1 != 0;
                    let ext = if neg { x | !mask } else { x }; // sign-extended
                    let n = count.min(bits);
                    carry = (ext >> (n - 1)) & 1 != 0;
                    let shifted = if count < bits {
                        ext >> count
                    } else if neg {
                        !0
                    } else {
                        0
                    };
                    res = shifted & mask;
                }
                if set_x {
                    xflag = carry;
                }
            }
        }
        SH_RO => {
            set_x = false;
            if count != 0 {
                let n = count % bits;
                res = if left {
                    ((x << n) | (x >> (bits - n))) & mask
                } else {
                    ((x >> n) | (x << (bits - n))) & mask
                };
                carry = if left {
                    res & 1 != 0
                } else {
                    (res >> (bits - 1)) & 1 != 0
                };
            }
        }
        _ => {
            // ROX: rotate through X, bits + 1 wide
            debug_assert_eq!(ty, SH_ROX);
            let n = count % (bits + 1);
            let mut w = x | u64::from(xflag) << bits;
            let wm = (mask << 1) | 1;
            if n != 0 {
                w = if left {
                    ((w << n) | (w >> (bits + 1 - n))) & wm
                } else {
                    ((w >> n) | (w << (bits + 1 - n))) & wm
                };
            }
            res = w & mask;
            xflag = (w >> bits) & 1 != 0;
            carry = xflag; // C = X, also for a count of zero
            set_x = true;
        }
    }

    let mut f = 0;
    if res == 0 {
        f |= CCR_Z;
    }
    if (res >> (bits - 1)) & 1 != 0 {
        f |= CCR_N;
    }
    if overflow {
        f |= CCR_V;
    }
    if carry {
        f |= CCR_C;
    }
    let mut m = CCR_N | CCR_Z | CCR_V | CCR_C;
    if set_x {
        m |= CCR_X;
        if xflag {
            f |= CCR_X;
        }
    }
    c.set_ccr_bits(m, f);
    res as u32
}

/// <op> #n,Dy / <op> Dx,Dy: 1110 ccc d ss i tt yyy. 6 + 2n cycles for bytes
/// and words, 8 + 2n for longs.
fn op_shift_reg(c: &mut Cpu, op: u16) -> Res {
    let size = size_field(op);
    let ccc = u32::from((op >> 9) & 7);
    let count = if op & 0x20 != 0 {
        c.r.d[reg9(op)] & 63
    } else if ccc != 0 {
        ccc
    } else {
        8
    };
    let dy = reg0(op);
    let v = shift(
        c,
        u32::from((op >> 3) & 3),
        op & 0x100 != 0,
        c.r.d[dy],
        count,
        size,
    );
    c.set_dreg(dy, v, size);
    c.cycles(if size == SZ_L { 4 } else { 2 } + 2 * count);
    Ok(())
}

/// <op> <ea>: a word in memory, shifted by one.
fn op_shift_mem(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let v = c.ea_read(&mut e)?;
    let r = shift(c, u32::from((op >> 9) & 3), op & 0x100 != 0, v, 1, SZ_W);
    c.ea_write(&mut e, r)
}

// ---- Bit operations --------------------------------------------------------------

const BIT_TST: u16 = 0;
const BIT_CHG: u16 = 1;
const BIT_CLR: u16 = 2;
const BIT_SET: u16 = 3;

/// Bit `n` of the operand: modulo 32 in a data register (long), modulo 8 in
/// memory (byte). Z is the bit's old value inverted.
fn bit_op(c: &mut Cpu, op: u16, n: u32) -> Res {
    let ty = (op >> 6) & 3;
    let mode = (op >> 3) & 7;
    let mut e = c.ea_decode_op(op, if mode == 0 { SZ_L } else { SZ_B })?;
    let mut v = c.ea_read(&mut e)?;
    let n = n & if mode == 0 { 31 } else { 7 };
    let bit = 1u32 << n;
    c.set_ccr_bits(CCR_Z, if v & bit != 0 { 0 } else { CCR_Z });
    match ty {
        BIT_CHG => v ^= bit,
        BIT_CLR => v &= !bit,
        BIT_SET => v |= bit,
        _ => {}
    }
    if mode == 0 {
        // BTST 6, BCHG/BSET 6 or 8, BCLR 8 or 10 (bits 16-31 take 2 more);
        // with an immediate bit number, 4 more for its word.
        let mut t = 2;
        if ty != BIT_TST {
            t += if ty == BIT_CLR { 2 } else { 0 } + if n >= 16 { 2 } else { 0 };
        }
        c.cycles(t);
    } else if e.kind == EaKind::Imm {
        c.cycles(2); // BTST Dn,#imm: 10, not the 8 of 4 + <ea>
    }
    if ty != BIT_TST {
        c.ea_write(&mut e, v)?;
    }
    Ok(())
}

/// B<op> Dn,<ea>: 0000 nnn 1tt eeeeee
fn op_bit_dyn(c: &mut Cpu, op: u16) -> Res {
    bit_op(c, op, c.r.d[reg9(op)])
}

/// B<op> #n,<ea>: 0000 1000 tt eeeeee, then the bit number.
fn op_bit_imm(c: &mut Cpu, op: u16) -> Res {
    let n = c.fetch16()? & 0xFF;
    bit_op(c, op, n)
}

// ---- BCD ---------------------------------------------------------------------------

/// d + s + X in BCD. The decimal carry decides on the low-corrected sum; V
/// is set when the correction turns bit 7 on (undefined in the manual, as in
/// the data); N is bit 7 of the result.
fn bcd_add(c: &mut Cpu, s: u32, d: u32) -> u32 {
    let x = u32::from(u32::from(c.r.sr) & CCR_X != 0);
    let bin = s + d + x;
    let mut res = bin;
    if (s & 0xF) + (d & 0xF) + x > 9 {
        res += 6;
    }
    let carry = res > 0x9F;
    if carry {
        res += 0x60;
    }
    let mut f = if res & 0x80 != 0 { CCR_N } else { 0 };
    if !bin & res & 0x80 != 0 {
        f |= CCR_V;
    }
    if carry {
        f |= CCR_C | CCR_X;
    }
    // Z: cleared if the result is nonzero, else unchanged
    let m = CCR_X | CCR_N | CCR_V | CCR_C | if res & 0xFF != 0 { CCR_Z } else { 0 };
    c.set_ccr_bits(m, f);
    res & 0xFF
}

/// d - s - X in BCD. The borrow (C, X) is that of the low-corrected
/// difference; the high correction is applied on the binary borrow. V is
/// set when the correction turns bit 7 off.
fn bcd_sub(c: &mut Cpu, s: u32, d: u32) -> u32 {
    let x = i32::from(u32::from(c.r.sr) & CCR_X != 0);
    let (s, d) = (s as i32, d as i32);
    let bin = d - s - x;
    let mut res = bin;
    if (d & 0xF) - (s & 0xF) - x < 0 {
        res -= 6;
    }
    let borrow = res < 0;
    if bin < 0 {
        res -= 0x60;
    }
    let r = res as u32 & 0xFF;
    let mut f = if r & 0x80 != 0 { CCR_N } else { 0 };
    if bin as u32 & !(res as u32) & 0x80 != 0 {
        f |= CCR_V;
    }
    if borrow {
        f |= CCR_C | CCR_X;
    }
    // Z as in bcd_add()
    let m = CCR_X | CCR_N | CCR_V | CCR_C | if r != 0 { CCR_Z } else { 0 };
    c.set_ccr_bits(m, f);
    r
}

/// ABCD/SBCD Dy,Dx and -(Ay),-(Ax): 1100/1000 xxx 10000 r yyy
fn bcd_xy<const ADD: bool>(c: &mut Cpu, op: u16) -> Res {
    let (x, y) = (reg9(op), reg0(op));
    let calc = |c: &mut Cpu, s, d| {
        if ADD {
            bcd_add(c, s, d)
        } else {
            bcd_sub(c, s, d)
        }
    };
    if op & 8 == 0 {
        let (s, d) = (c.r.d[y] & 0xFF, c.r.d[x] & 0xFF);
        let r = calc(c, s, d);
        c.set_dreg(x, r, SZ_B);
        c.cycles(2);
        return Ok(());
    }
    // -(Ay),-(Ax): 18 cycles, one internal predecrement step
    c.cycles(2);
    c.ae_pc = c.r.pc;
    c.r.a[y] = c.r.a[y].wrapping_sub(if y == 7 { 2 } else { 1 });
    let s = c.read8(c.r.a[y], ACC_DATA)?;
    c.r.a[x] = c.r.a[x].wrapping_sub(if x == 7 { 2 } else { 1 });
    let d = c.read8(c.r.a[x], ACC_DATA)?;
    let r = calc(c, s, d);
    c.write8(c.r.a[x], r)
}

/// NBCD <ea>: 0 - <ea> - X
fn op_nbcd(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_B)?;
    let v = c.ea_read(&mut e)?;
    if e.kind == EaKind::DReg {
        c.cycles(2);
    }
    let r = bcd_sub(c, v, 0);
    c.ea_write(&mut e, r)
}

// ---- Table -------------------------------------------------------------------------

pub fn fill(tb: &mut OpTable) {
    // Shifts and rotates on registers: 1110 ccc d ss i tt yyy, ss != 11
    for s in 0..3u16 {
        tb.fill(0xE000 | s << 6, 0x0F3F, 0, op_shift_reg);
    }
    // On memory: 1110 0tt d 11 eeeeee
    tb.fill(0xE0C0, 0x073F, EA_MEM_ALT, op_shift_mem);

    // BTST Dn,<ea> takes any data mode, #imm included; the others data
    // alterable ones. BTST #n,<ea>: data modes but #imm.
    tb.fill(0x0100, 0x0E3F, EA_DATA, op_bit_dyn);
    tb.fill(0x0800, 0x003F, EA_DATA & !EAB_IMM, op_bit_imm);
    for t in BIT_CHG..=BIT_SET {
        tb.fill(0x0100 | t << 6, 0x0E3F, EA_DATA_ALT, op_bit_dyn);
        tb.fill(0x0800 | t << 6, 0x003F, EA_DATA_ALT, op_bit_imm);
    }

    tb.fill(0xC100, 0x0E0F, 0, bcd_xy::<true>);
    tb.fill(0x8100, 0x0E0F, 0, bcd_xy::<false>);
    tb.fill(0x4800, 0x003F, EA_DATA_ALT, op_nbcd);
}
