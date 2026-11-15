// SPDX-License-Identifier: MPL-2.0
//! Bit fields of the 68020 and later (M68000PRM 4, MC68020UM): BFTST,
//! BFEXTU, BFEXTS, BFCHG, BFCLR, BFSET, BFFFO and BFINS.
//!
//! Extension word (it comes before the operand's own words):
//!
//! ```text
//!   bits 14-12  Dn of BFEXTU, BFEXTS, BFFFO and BFINS
//!   bit 11      Do: the offset is in Dn (bits 8-6), else bits 10-6
//!   bit 5       Dw: the width is in Dn (bits 2-0), else bits 4-0
//! ```
//!
//! The width is taken modulo 32, 0 meaning 32. Bit 0 of a field is its most
//! significant bit. In a data register the offset is taken modulo 32 and
//! the field wraps from bit 0 round to bit 31. In memory the offset is
//! signed (-2^31 to 2^31-1 from Dn, 0-31 immediate) and counts from bit 7
//! of the byte at <ea>; the field may cover up to 5 bytes.
//!
//! Flags, from the field before the change (BFINS: from the value
//! inserted): N its top bit, Z if it is all zeros, V and C cleared, X kept.

use super::{
    CCR_C, CCR_N, CCR_V, CCR_Z, EA_CONTROL, EA_CONTROL_ALT, EAB_DREG, Ea, EaKind, Handler, OpTable,
    SZ_B,
};
use crate::cpu::{Cpu, Res};

const BF_TST: u32 = 0;
const BF_EXTU: u32 = 1;
const BF_CHG: u32 = 2;
const BF_EXTS: u32 = 3;
const BF_CLR: u32 = 4;
const BF_FFO: u32 = 5;
const BF_SET: u32 = 6;
const BF_INS: u32 = 7;

/// Low `width` (1-32) bits set.
fn bf_mask(width: u32) -> u32 {
    if width == 32 {
        0xFFFF_FFFF
    } else {
        (1 << width) - 1
    }
}

/// Where the field is and its contents, right-aligned.
struct Bitfield {
    e: Ea,
    offset: i32,
    width: u32,
    /// memory: first byte
    addr: u32,
    /// memory: bytes covered
    bytes: u32,
    /// memory: bits below the field in those bytes
    shift: u32,
    /// memory: those bytes
    window: u64,
    value: u32,
}

fn bf_read(c: &mut Cpu, op: u16, ext: u32) -> Res<Bitfield> {
    let offset = if ext & 0x0800 != 0 {
        c.r.d[((ext >> 6) & 7) as usize] as i32
    } else {
        ((ext >> 6) & 31) as i32
    };
    let w = if ext & 0x0020 != 0 {
        c.r.d[(ext & 7) as usize] & 31
    } else {
        ext & 31
    };
    let width = if w == 0 { 32 } else { w };
    let e = c.ea_decode_op(op, SZ_B)?;
    let mut bf = Bitfield {
        e,
        offset,
        width,
        addr: 0,
        bytes: 0,
        shift: 0,
        window: 0,
        value: 0,
    };

    if e.kind == EaKind::DReg {
        let rot = c.r.d[(e.reg & 7) as usize].rotate_left(offset as u32 & 31);
        bf.value = rot >> (32 - width);
        return Ok(bf);
    }
    // byte offset: the signed offset divided by 8, rounded down
    let byte_off = (offset >> 3) as u32;
    let bit = offset as u32 & 7;
    bf.addr = e.addr.wrapping_add(byte_off);
    bf.bytes = (bit + width).div_ceil(8);
    bf.shift = 8 * bf.bytes - bit - width;
    for i in 0..bf.bytes {
        let b = c.read8(bf.addr.wrapping_add(i), e.acc)?;
        bf.window = bf.window << 8 | u64::from(b);
    }
    bf.value = (bf.window >> bf.shift) as u32 & bf_mask(width);
    Ok(bf)
}

fn bf_write(c: &mut Cpu, bf: &Bitfield, v: u32) -> Res {
    let m = bf_mask(bf.width);
    let v = v & m;
    if bf.e.kind == EaKind::DReg {
        let rot = bf.offset as u32 & 31;
        let r = (bf.e.reg & 7) as usize;
        let d = c.r.d[r].rotate_left(rot);
        let low = 32 - bf.width;
        let d = (d & !(m << low)) | v << low;
        c.r.d[r] = d.rotate_right(rot);
        return Ok(());
    }
    let w = (bf.window & !(u64::from(m) << bf.shift)) | u64::from(v) << bf.shift;
    for i in 0..bf.bytes {
        let b = (w >> (8 * (bf.bytes - 1 - i))) as u32 & 0xFF;
        c.write8(bf.addr.wrapping_add(i), b)?;
    }
    Ok(())
}

fn bf_flags(c: &mut Cpu, v: u32, width: u32) {
    let mut f = 0;
    if v & bf_mask(width) == 0 {
        f |= CCR_Z;
    }
    if v & (1 << (width - 1)) != 0 {
        f |= CCR_N;
    }
    c.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, f);
}

fn bitfield_op<const KIND: u32>(c: &mut Cpu, op: u16) -> Res {
    let ext = c.fetch16()?;
    let dn = ((ext >> 12) & 7) as usize;
    let bf = bf_read(c, op, ext)?;
    let v = bf.value;
    let m = bf_mask(bf.width);
    c.cycles(if bf.e.kind == EaKind::DReg { 4 } else { 8 });

    if KIND == BF_INS {
        let ins = c.r.d[dn] & m;
        bf_flags(c, ins, bf.width);
        return bf_write(c, &bf, ins);
    }
    bf_flags(c, v, bf.width);
    match KIND {
        BF_EXTU => c.r.d[dn] = v,
        BF_EXTS => {
            c.r.d[dn] = if v & (1 << (bf.width - 1)) != 0 {
                v | !m
            } else {
                v
            }
        }
        BF_FFO => {
            // offset of the first 1 from the top of the field; offset +
            // width when there is none
            let mut i = 0;
            while i < bf.width && v & (1 << (bf.width - 1 - i)) == 0 {
                i += 1;
            }
            c.r.d[dn] = (bf.offset as u32).wrapping_add(i);
        }
        BF_CHG => bf_write(c, &bf, !v)?,
        BF_CLR => bf_write(c, &bf, 0)?,
        BF_SET => bf_write(c, &bf, m)?,
        _ => {} // BF_TST
    }
    Ok(())
}

pub fn fill(tb: &mut OpTable) {
    // 1110 1ttt 11 <ea>: the ones that only read take control modes, the
    // others control alterable ones; all take Dn.
    const H: [Handler; 8] = [
        bitfield_op::<BF_TST>,
        bitfield_op::<BF_EXTU>,
        bitfield_op::<BF_CHG>,
        bitfield_op::<BF_EXTS>,
        bitfield_op::<BF_CLR>,
        bitfield_op::<BF_FFO>,
        bitfield_op::<BF_SET>,
        bitfield_op::<BF_INS>,
    ];
    for t in 0..8u32 {
        let reads_only = matches!(t, BF_TST | BF_EXTU | BF_EXTS | BF_FFO);
        let classes = EAB_DREG
            | if reads_only {
                EA_CONTROL
            } else {
                EA_CONTROL_ALT
            };
        tb.fill(0xE8C0 | (t as u16) << 8, 0x003F, classes, H[t as usize]);
    }
}
