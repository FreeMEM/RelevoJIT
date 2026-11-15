// SPDX-License-Identifier: MPL-2.0
//! Arithmetic and logic: ADD, ADDA, ADDI, ADDQ, ADDX, SUB, SUBA, SUBI, SUBQ,
//! SUBX, CMP, CMPA, CMPI, CMPM, AND, ANDI, OR, ORI, EOR, EORI, NEG, NEGX and
//! NOT. (ANDI, ORI and EORI to CCR and SR are in system.rs.)
//!
//! Cycles: the 68000 times of M68000PRM chapter 8 minus 4 per word and per
//! access, which count themselves (see mod.rs).

use super::{
    CCR_C, CCR_N, CCR_V, CCR_X, CCR_Z, EA_ALL, EA_ALTERABLE, EA_DATA, EA_DATA_ALT, EA_MEM_ALT, Ea,
    EaKind, Handler, OpTable, SZ_B, SZ_L, SZ_W, reg0, reg9, sext, sz_mask, sz_msb,
};
use crate::cpu::{ACC_DATA, Cpu, Res};

// ---- Flags -------------------------------------------------------------------

const ALU_ADD: u32 = 0;
const ALU_SUB: u32 = 1;
const ALU_CMP: u32 = 2;
const ALU_AND: u32 = 3;
const ALU_OR: u32 = 4;
const ALU_EOR: u32 = 5;

/// d + s + x: sets X, N, Z, V and C. With `sticky_z` (ADDX), Z is only
/// cleared, never set.
#[inline(always)]
fn add_flags(c: &mut Cpu, d: u32, s: u32, x: u32, size: u32, sticky_z: bool) -> u32 {
    let (m, msb) = (sz_mask(size), sz_msb(size));
    let (d, s) = (d & m, s & m);
    let r = d.wrapping_add(s).wrapping_add(x) & m;
    let mut f = 0;
    if r & msb != 0 {
        f |= CCR_N;
    }
    if (s ^ r) & (d ^ r) & msb != 0 {
        f |= CCR_V;
    }
    // carry out of the top bit
    if ((s & d) | ((s | d) & !r)) & msb != 0 {
        f |= CCR_C | CCR_X;
    }
    if sticky_z {
        if r == 0 {
            f |= u32::from(c.r.sr) & CCR_Z;
        }
    } else if r == 0 {
        f |= CCR_Z;
    }
    c.set_ccr_bits(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C, f);
    r
}

/// d - s - x: sets N, Z, V and C, and X too unless `cmp`. With `sticky_z`
/// (SUBX, NEGX), Z is only cleared, never set.
#[inline(always)]
fn sub_flags(c: &mut Cpu, d: u32, s: u32, x: u32, size: u32, cmp: bool, sticky_z: bool) -> u32 {
    let (m, msb) = (sz_mask(size), sz_msb(size));
    let (d, s) = (d & m, s & m);
    let r = d.wrapping_sub(s).wrapping_sub(x) & m;
    let mut f = 0;
    if r & msb != 0 {
        f |= CCR_N;
    }
    if (s ^ d) & (r ^ d) & msb != 0 {
        f |= CCR_V;
    }
    // borrow into the top bit
    if ((s & !d) | ((s | !d) & r)) & msb != 0 {
        f |= CCR_C | CCR_X;
    }
    if sticky_z {
        if r == 0 {
            f |= u32::from(c.r.sr) & CCR_Z;
        }
    } else if r == 0 {
        f |= CCR_Z;
    }
    let mask = CCR_N | CCR_Z | CCR_V | CCR_C | if cmp { 0 } else { CCR_X };
    c.set_ccr_bits(mask, f);
    r
}

/// The operation of the two-operand instructions, with their flags.
#[inline(always)]
fn alu(c: &mut Cpu, op: u32, d: u32, s: u32, size: u32) -> u32 {
    let r = match op {
        ALU_ADD => return add_flags(c, d, s, 0, size, false),
        ALU_SUB => return sub_flags(c, d, s, 0, size, false, false),
        ALU_CMP => return sub_flags(c, d, s, 0, size, true, false),
        ALU_AND => d & s,
        ALU_OR => d | s,
        _ => d ^ s,
    };
    c.set_nz(r, size);
    r
}

// ---- <ea>,Dn and Dn,<ea> -------------------------------------------------------

/// ADD, SUB, AND, OR, CMP <ea>,Dn. Long takes 2 internal cycles more, 4 with
/// a register or immediate source (CMP: always 2).
fn ea_to_dreg<const ALU: u32, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let dn = reg9(op);
    let mut e = c.ea_decode_op(op, SIZE)?;
    let s = c.ea_read(&mut e)?;
    let r = alu(c, ALU, c.r.d[dn], s, SIZE);
    if ALU != ALU_CMP {
        c.set_dreg(dn, r, SIZE);
    }
    if SIZE == SZ_L {
        c.cycles(if ALU == ALU_CMP || e.kind == EaKind::Mem {
            2
        } else {
            4
        });
    }
    Ok(())
}

/// ADD, SUB, AND, OR, EOR Dn,<ea>: read, change, write. EOR also comes here
/// with a data register destination (long: 4 internal cycles).
fn dreg_to_ea<const ALU: u32, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let dn = reg9(op);
    let mut e = c.ea_decode_op(op, SIZE)?;
    let d = c.ea_read(&mut e)?;
    let r = alu(c, ALU, d, c.r.d[dn], SIZE);
    if e.kind == EaKind::DReg && SIZE == SZ_L {
        c.cycles(4);
    }
    c.ea_write(&mut e, r)
}

// ---- ADDA, SUBA, CMPA -----------------------------------------------------------

/// Opmode 011 is word (sign-extended), 111 long. The whole An changes; no
/// flags.
#[inline(always)]
fn read_addr_src(c: &mut Cpu, op: u16) -> Res<(u32, Ea)> {
    let size = if op & 0x0100 != 0 { SZ_L } else { SZ_W };
    let mut e = c.ea_decode_op(op, size)?;
    let v = c.ea_read(&mut e)?;
    Ok((sext(v, size), e))
}

/// ADDA/SUBA: word 4 internal cycles; long 2, 4 with a register or
/// immediate source.
fn adda_cycles(c: &mut Cpu, op: u16, e: &Ea) {
    c.cycles(if op & 0x0100 == 0 || e.kind != EaKind::Mem {
        4
    } else {
        2
    });
}

fn op_adda(c: &mut Cpu, op: u16) -> Res {
    let (s, e) = read_addr_src(c, op)?;
    c.r.a[reg9(op)] = c.r.a[reg9(op)].wrapping_add(s);
    adda_cycles(c, op, &e);
    Ok(())
}

fn op_suba(c: &mut Cpu, op: u16) -> Res {
    let (s, e) = read_addr_src(c, op)?;
    c.r.a[reg9(op)] = c.r.a[reg9(op)].wrapping_sub(s);
    adda_cycles(c, op, &e);
    Ok(())
}

fn op_cmpa(c: &mut Cpu, op: u16) -> Res {
    let (s, _) = read_addr_src(c, op)?;
    sub_flags(c, c.r.a[reg9(op)], s, 0, SZ_L, true, false);
    c.cycles(2);
    Ok(())
}

// ---- Immediate: ADDI, SUBI, CMPI, ANDI, ORI, EORI -------------------------------

fn immediate<const ALU: u32, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let s = if SIZE == SZ_L {
        c.fetch32()?
    } else {
        c.fetch16()? & sz_mask(SIZE)
    };
    let mut e = c.ea_decode_op(op, SIZE)?;
    let d = c.ea_read(&mut e)?;
    let r = alu(c, ALU, d, s, SIZE);
    if e.kind == EaKind::DReg && SIZE == SZ_L {
        c.cycles(if ALU == ALU_CMP { 2 } else { 4 });
    }
    if ALU != ALU_CMP {
        c.ea_write(&mut e, r)?;
    }
    Ok(())
}

// ---- ADDQ, SUBQ -------------------------------------------------------------------

/// #1-8 (0 is 8). To An, the whole register changes and the flags do not;
/// Dn.L and An take 4 internal cycles.
fn quick<const SUB: bool, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let mut q = u32::from((op >> 9) & 7);
    if q == 0 {
        q = 8;
    }
    if (op >> 3) & 7 == 1 {
        let an = reg0(op);
        c.r.a[an] = if SUB {
            c.r.a[an].wrapping_sub(q)
        } else {
            c.r.a[an].wrapping_add(q)
        };
        c.cycles(4);
        return Ok(());
    }
    let mut e = c.ea_decode_op(op, SIZE)?;
    let d = c.ea_read(&mut e)?;
    let r = alu(c, if SUB { ALU_SUB } else { ALU_ADD }, d, q, SIZE);
    if e.kind == EaKind::DReg && SIZE == SZ_L {
        c.cycles(4);
    }
    c.ea_write(&mut e, r)
}

// ---- ADDX, SUBX -------------------------------------------------------------------

/// -(An) for ADDX and SUBX. A long is read low word first, at An-2, and An
/// only changes once both words are in (SingleStepTests data).
fn read_predec(c: &mut Cpu, reg: usize, size: u32) -> Res<u32> {
    let addr = c.r.a[reg];
    if size == SZ_L {
        let lo = c.read16(addr.wrapping_sub(2), ACC_DATA)?;
        let hi = c.read16(addr.wrapping_sub(4), ACC_DATA)?;
        c.r.a[reg] = addr.wrapping_sub(4);
        return Ok(hi << 16 | lo);
    }
    let addr = addr.wrapping_sub(if size == SZ_B && reg == 7 { 2 } else { size });
    c.r.a[reg] = addr;
    c.read(addr, size, ACC_DATA)
}

fn extended<const SUB: bool, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let (rx, ry) = (reg9(op), reg0(op));
    let x = u32::from(u32::from(c.r.sr) & CCR_X != 0);
    if op & 8 == 0 {
        // Dy,Dx
        let (d, s) = (c.r.d[rx], c.r.d[ry]);
        let r = if SUB {
            sub_flags(c, d, s, x, SIZE, false, true)
        } else {
            add_flags(c, d, s, x, SIZE, true)
        };
        c.set_dreg(rx, r, SIZE);
        if SIZE == SZ_L {
            c.cycles(4);
        }
        return Ok(());
    }
    // -(Ay),-(Ax): 2 internal cycles, then the reads; a fault stacks the PC
    // one word on.
    c.cycles(2);
    c.ae_pc = c.r.pc.wrapping_add(2);
    let s = read_predec(c, ry, SIZE)?;
    let d = read_predec(c, rx, SIZE)?;
    let r = if SUB {
        sub_flags(c, d, s, x, SIZE, false, true)
    } else {
        add_flags(c, d, s, x, SIZE, true)
    };
    let addr = c.r.a[rx];
    if SIZE == SZ_L {
        c.write16(addr.wrapping_add(2), r & 0xFFFF)?;
        c.write16(addr, r >> 16)
    } else {
        c.write(addr, r, SIZE)
    }
}

// ---- CMPM (Ay)+,(Ax)+ ----------------------------------------------------------------

/// If a read faults (SingleStepTests data): the stacked PC is one word on;
/// Ay has moved by 2 (word and long alike), Ax has not moved.
fn op_cmpm<const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let (ry, rx) = (reg0(op), reg9(op));
    c.ae_pc = c.r.pc.wrapping_add(2);
    let addr = c.r.a[ry];
    if SIZE != SZ_B && addr & 1 != 0 {
        c.r.a[ry] = c.r.a[ry].wrapping_add(2);
    }
    let step = if SIZE == SZ_B && ry == 7 { 2 } else { SIZE };
    let s = c.read(addr, SIZE, ACC_DATA)?;
    c.r.a[ry] = addr.wrapping_add(step);
    let step = if SIZE == SZ_B && rx == 7 { 2 } else { SIZE };
    let addr = c.r.a[rx];
    let d = c.read(addr, SIZE, ACC_DATA)?;
    c.r.a[rx] = addr.wrapping_add(step);
    sub_flags(c, d, s, 0, SIZE, true, false);
    Ok(())
}

// ---- NEG, NEGX, NOT ----------------------------------------------------------------

const UN_NEGX: u32 = 0;
const UN_NEG: u32 = 1;
const UN_NOT: u32 = 2;

/// Dn.L takes 2 internal cycles.
fn unary<const KIND: u32, const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SIZE)?;
    let d = c.ea_read(&mut e)?;
    let r = match KIND {
        UN_NEGX => {
            let x = u32::from(u32::from(c.r.sr) & CCR_X != 0);
            sub_flags(c, 0, d, x, SIZE, false, true)
        }
        UN_NEG => sub_flags(c, 0, d, 0, SIZE, false, false),
        _ => {
            let r = !d;
            c.set_nz(r, SIZE);
            r
        }
    };
    if e.kind == EaKind::DReg && SIZE == SZ_L {
        c.cycles(2);
    }
    c.ea_write(&mut e, r)
}

// ---- Table -------------------------------------------------------------------------

/// The line 8/9/B/C/D layout: register in bits 11-9, opmode in 8-6.
fn fill_line(tb: &mut OpTable, line: u16, opmode: u16, classes: u32, h: Handler) {
    tb.fill(line | opmode << 6, 0x0E3F, classes, h);
}

/// The handler of `$f::<ALU, SIZE>` for the size field `sz` (0 byte, 1 word,
/// 2 long).
macro_rules! by_size {
    ($f:ident, $p:expr, $sz:expr) => {
        match $sz {
            0 => $f::<{ $p }, SZ_B> as Handler,
            1 => $f::<{ $p }, SZ_W> as Handler,
            _ => $f::<{ $p }, SZ_L> as Handler,
        }
    };
}

pub fn fill(tb: &mut OpTable) {
    for sz in 0..3u16 {
        let arith_src = if sz != 0 { EA_ALL } else { EA_DATA };
        fill_line(tb, 0xD000, sz, arith_src, by_size!(ea_to_dreg, ALU_ADD, sz));
        fill_line(
            tb,
            0xD000,
            4 + sz,
            EA_MEM_ALT,
            by_size!(dreg_to_ea, ALU_ADD, sz),
        );
        fill_line(tb, 0x9000, sz, arith_src, by_size!(ea_to_dreg, ALU_SUB, sz));
        fill_line(
            tb,
            0x9000,
            4 + sz,
            EA_MEM_ALT,
            by_size!(dreg_to_ea, ALU_SUB, sz),
        );
        fill_line(tb, 0xC000, sz, EA_DATA, by_size!(ea_to_dreg, ALU_AND, sz));
        fill_line(
            tb,
            0xC000,
            4 + sz,
            EA_MEM_ALT,
            by_size!(dreg_to_ea, ALU_AND, sz),
        );
        fill_line(tb, 0x8000, sz, EA_DATA, by_size!(ea_to_dreg, ALU_OR, sz));
        fill_line(
            tb,
            0x8000,
            4 + sz,
            EA_MEM_ALT,
            by_size!(dreg_to_ea, ALU_OR, sz),
        );
    }
    for sz in 0..3u16 {
        fill_line(
            tb,
            0xB000,
            sz,
            if sz != 0 { EA_ALL } else { EA_DATA },
            by_size!(ea_to_dreg, ALU_CMP, sz),
        );
        fill_line(
            tb,
            0xB000,
            4 + sz,
            EA_DATA_ALT,
            by_size!(dreg_to_ea, ALU_EOR, sz),
        );
        // ADDX/SUBX: Dy,Dx and -(Ay),-(Ax); CMPM (Ay)+,(Ax)+
        let x = 0x0100 | sz << 6;
        tb.fill(0xD000 | x, 0x0E0F, 0, by_size!(extended, false, sz));
        tb.fill(0x9000 | x, 0x0E0F, 0, by_size!(extended, true, sz));
        let cmpm = match sz {
            0 => op_cmpm::<SZ_B> as Handler,
            1 => op_cmpm::<SZ_W>,
            _ => op_cmpm::<SZ_L>,
        };
        tb.fill(0xB008 | x, 0x0E07, 0, cmpm);

        let s = sz << 6;
        tb.fill(s, 0x003F, EA_DATA_ALT, by_size!(immediate, ALU_OR, sz));
        tb.fill(
            0x0200 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(immediate, ALU_AND, sz),
        );
        tb.fill(
            0x0400 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(immediate, ALU_SUB, sz),
        );
        tb.fill(
            0x0600 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(immediate, ALU_ADD, sz),
        );
        tb.fill(
            0x0A00 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(immediate, ALU_EOR, sz),
        );
        // CMPI takes PC-relative operands from the 68020 on (RR2).
        tb.fill(
            0x0C00 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(immediate, ALU_CMP, sz),
        );

        let q = if sz != 0 { EA_ALTERABLE } else { EA_DATA_ALT };
        tb.fill(0x5000 | s, 0x0E3F, q, by_size!(quick, false, sz));
        tb.fill(0x5100 | s, 0x0E3F, q, by_size!(quick, true, sz));

        tb.fill(
            0x4000 | s,
            0x003F,
            EA_DATA_ALT,
            by_size!(unary, UN_NEGX, sz),
        );
        tb.fill(0x4400 | s, 0x003F, EA_DATA_ALT, by_size!(unary, UN_NEG, sz));
        tb.fill(0x4600 | s, 0x003F, EA_DATA_ALT, by_size!(unary, UN_NOT, sz));
    }
    fill_line(tb, 0x9000, 3, EA_ALL, op_suba);
    fill_line(tb, 0x9000, 7, EA_ALL, op_suba);
    fill_line(tb, 0xD000, 3, EA_ALL, op_adda);
    fill_line(tb, 0xD000, 7, EA_ALL, op_adda);
    fill_line(tb, 0xB000, 3, EA_ALL, op_cmpa);
    fill_line(tb, 0xB000, 7, EA_ALL, op_cmpa);
}
