// SPDX-License-Identifier: MPL-2.0
//! Data movement: MOVE, MOVEA, MOVEQ, MOVEM, MOVEP, LEA, PEA, EXG, SWAP,
//! LINK, UNLK, CLR, TST, EXT and TAS.

use super::{
    CCR_N, CCR_Z, EA_ALL, EA_CONTROL, EA_CONTROL_ALT, EA_DATA, EA_DATA_ALT, EAB_POSTINC,
    EAB_PREDEC, Ea, EaKind, Handler, OpTable, SZ_B, SZ_L, SZ_W, ea_valid, reg0, reg9, sext,
    size_field,
};
use crate::cpu::{ACC_DATA, Cpu, Res};

// ---- MOVE and MOVEA ---------------------------------------------------------

/// MOVE.L writes its destination before its flags are complete. If that
/// write faults, the 68000 has stacked an SR whose N and Z depend on how far
/// it got (SingleStepTests data):
fn move_long_fault_flags(c: &mut Cpu, src: &Ea, dst: &Ea, v: u32) {
    let abs_l = dst.is_mode7(1);
    let ind = dst.mode == 2 || dst.mode == 3;
    if src.kind == EaKind::Mem {
        // the low word read last decides
        if ind || abs_l {
            c.set_nz(v & 0xFFFF, SZ_W);
        } else {
            c.set_nz(v, SZ_L);
        }
    } else if ind {
        // nothing set yet
    } else if dst.mode == 5 || dst.mode == 6 {
        // N and Z only
        let f = if v == 0 { CCR_Z } else { 0 } | if v & 0x8000_0000 != 0 { CCR_N } else { 0 };
        c.set_ccr_bits(CCR_N | CCR_Z, f);
    } else {
        c.set_nz(v, SZ_L);
    }
}

#[inline(always)]
fn op_move<const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let mut s = c.ea_decode_op(op, SIZE)?;
    let v = c.ea_read(&mut s)?;
    let pc = c.r.pc;
    let mut d = c.ea_decode(u32::from((op >> 6) & 7), u32::from((op >> 9) & 7), SIZE)?;
    if d.kind == EaKind::Mem {
        // The next prefetch happens before the write: the stacked PC is one
        // word further on, two for (xxx).L after a source that is not in
        // memory.
        let abs_l = d.is_mode7(1);
        d.ae_pc = pc.wrapping_add(if abs_l && s.kind != EaKind::Mem { 4 } else { 2 });
        // If the write faults, the 68000's prefetches are one word ahead of
        // the words counted here for -(An), one behind for (xxx).L after a
        // memory source.
        if d.mode == 4 {
            c.fault_cycles = 4;
        } else if abs_l && s.kind == EaKind::Mem {
            c.fault_cycles = -4;
        }
        if SIZE == SZ_L {
            move_long_fault_flags(c, &s, &d, v);
        } else {
            c.set_nz(v, SIZE);
            // MOVE.W to -(An) has already loaded the next opcode.
            if d.mode == 4 {
                c.ir = c.mem_read16(c.r.pc) as u16;
            }
        }
    }
    c.ea_write(&mut d, v)?;
    c.ir = op;
    c.set_nz(v, SIZE);
    Ok(())
}

fn op_movea_w(c: &mut Cpu, op: u16) -> Res {
    let mut s = c.ea_decode_op(op, SZ_W)?;
    c.r.a[reg9(op)] = sext(c.ea_read(&mut s)?, SZ_W);
    Ok(())
}

fn op_movea_l(c: &mut Cpu, op: u16) -> Res {
    let mut s = c.ea_decode_op(op, SZ_L)?;
    c.r.a[reg9(op)] = c.ea_read(&mut s)?;
    Ok(())
}

fn op_moveq(c: &mut Cpu, op: u16) -> Res {
    let v = sext(u32::from(op), SZ_B);
    c.r.d[reg9(op)] = v;
    c.set_nz(v, SZ_L);
    Ok(())
}

// ---- MOVEM -------------------------------------------------------------------

/// Register number n: 0-7 D0-D7, 8-15 A0-A7.
fn reg_n(c: &Cpu, n: usize) -> u32 {
    if n < 8 { c.r.d[n] } else { c.r.a[n & 7] }
}

fn set_reg_n(c: &mut Cpu, n: usize, v: u32) {
    if n < 8 {
        c.r.d[n] = v;
    } else {
        c.r.a[n & 7] = v;
    }
}

/// Registers to memory. The mask is read first, then the address's
/// extension words; the transfers come after the next prefetch.
fn op_movem_to_mem<const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let mask = c.fetch16()?;
    let mode = u32::from((op >> 3) & 7);
    let an = reg0(op);
    if mode == 4 {
        // -(An): A7 down to D0 (mask bit 0 is A7); An itself is stored with
        // its value before the instruction.
        let mut addr = c.r.a[an];
        c.ae_pc = c.r.pc.wrapping_add(2);
        for i in 0..16 {
            if mask & (1 << i) == 0 {
                continue;
            }
            let v = reg_n(c, 15 - i);
            addr = addr.wrapping_sub(SIZE);
            if SIZE == SZ_L {
                c.write16(addr.wrapping_add(2), v & 0xFFFF)?;
                c.write16(addr, v >> 16)?;
            } else {
                c.write16(addr, v)?;
            }
        }
        c.r.a[an] = addr;
        return Ok(());
    }
    let e = c.ea_decode(mode, an as u32, SIZE)?;
    let mut addr = e.addr;
    c.ae_pc = c.r.pc.wrapping_add(2);
    for i in 0..16 {
        if mask & (1 << i) == 0 {
            continue;
        }
        let v = reg_n(c, i);
        c.write(addr, v, SIZE)?;
        addr = addr.wrapping_add(SIZE);
    }
    Ok(())
}

/// Memory to registers, D0 to A7; words are sign-extended to 32 bits. With
/// (An)+, An ends at the address after the last one, even if it was in the
/// list.
fn op_movem_to_regs<const SIZE: u32>(c: &mut Cpu, op: u16) -> Res {
    let mask = c.fetch16()?;
    let e = c.ea_decode_op(op, SIZE)?;
    let mut addr = e.addr;
    c.ae_pc = c.r.pc.wrapping_add(2);
    for i in 0..16 {
        if mask & (1 << i) == 0 {
            continue;
        }
        let v = c.read(addr, SIZE, e.acc)?;
        set_reg_n(c, i, if SIZE == SZ_W { sext(v, SZ_W) } else { v });
        addr = addr.wrapping_add(SIZE);
    }
    c.cycles(4); // the 68000 reads one word more
    if e.mode == 3 {
        c.r.a[(e.reg & 7) as usize] = addr;
    }
    Ok(())
}

// ---- MOVEP: bytes at every other address --------------------------------

fn op_movep(c: &mut Cpu, op: u16) -> Res {
    let dn = reg9(op);
    let addr = c.r.a[reg0(op)].wrapping_add(sext(c.fetch16()?, SZ_W));
    let bytes = if op & 0x40 != 0 { 4 } else { 2 };
    if op & 0x80 != 0 {
        let v = c.r.d[dn];
        for i in 0..bytes {
            c.write8(addr.wrapping_add(2 * i), v >> (8 * (bytes - 1 - i)))?;
        }
    } else {
        let mut v = 0;
        for i in 0..bytes {
            v = v << 8 | c.read8(addr.wrapping_add(2 * i), ACC_DATA)?;
        }
        c.set_dreg(dn, v, bytes);
    }
    Ok(())
}

// ---- Addresses ---------------------------------------------------------------

fn op_lea(c: &mut Cpu, op: u16) -> Res {
    let e = c.ea_decode_op(op, SZ_L)?;
    if e.mode == 6 || e.is_mode7(3) {
        c.cycles(2);
    }
    c.r.a[reg9(op)] = e.addr;
    Ok(())
}

fn op_pea(c: &mut Cpu, op: u16) -> Res {
    let e = c.ea_decode_op(op, SZ_L)?;
    if e.mode == 6 || e.is_mode7(3) {
        c.cycles(2);
    }
    c.push32(e.addr)
}

/// LINK An,#d16: push An, An = SP, SP += d16.
fn op_link(c: &mut Cpu, op: u16) -> Res {
    let n = reg0(op);
    let d = sext(c.fetch16()?, SZ_W);
    c.push32(c.r.a[n])?; // LINK A7 stores SP before the push
    c.r.a[n] = c.r.a[7];
    c.r.a[7] = c.r.a[7].wrapping_add(d);
    Ok(())
}

/// UNLK An: SP = An, An = pop.
fn op_unlk(c: &mut Cpu, op: u16) -> Res {
    let n = reg0(op);
    let addr = c.r.a[n];
    c.ae_pc = c.r.pc.wrapping_add(2);
    let v = c.read32(addr, ACC_DATA)?;
    c.r.a[7] = addr.wrapping_add(4);
    c.r.a[n] = v;
    Ok(())
}

// ---- Registers ---------------------------------------------------------------

fn op_exg(c: &mut Cpu, op: u16) -> Res {
    let (x, y) = (reg9(op), reg0(op));
    match (op >> 3) & 0x1F {
        0x08 => c.r.d.swap(x, y),
        0x09 => c.r.a.swap(x, y),
        _ => core::mem::swap(&mut c.r.d[x], &mut c.r.a[y]),
    }
    c.cycles(2);
    Ok(())
}

fn op_swap(c: &mut Cpu, op: u16) -> Res {
    let v = c.r.d[reg0(op)].rotate_left(16);
    c.r.d[reg0(op)] = v;
    c.set_nz(v, SZ_L);
    Ok(())
}

fn op_ext_w(c: &mut Cpu, op: u16) -> Res {
    let v = sext(c.r.d[reg0(op)], SZ_B);
    c.set_dreg(reg0(op), v, SZ_W);
    c.set_nz(v, SZ_W);
    Ok(())
}

fn op_ext_l(c: &mut Cpu, op: u16) -> Res {
    let v = sext(c.r.d[reg0(op)], SZ_W);
    c.r.d[reg0(op)] = v;
    c.set_nz(v, SZ_L);
    Ok(())
}

// ---- CLR, TST, TAS -----------------------------------------------------------

/// CLR: the 68000 reads the operand before clearing it.
fn op_clr(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, size_field(op))?;
    c.ea_read(&mut e)?;
    if e.kind == EaKind::DReg && e.size == SZ_L {
        c.cycles(2);
    }
    c.ea_write(&mut e, 0)?;
    c.set_nz(0, SZ_B);
    Ok(())
}

fn op_tst(c: &mut Cpu, op: u16) -> Res {
    let size = size_field(op);
    let mut e = c.ea_decode_op(op, size)?;
    let v = c.ea_read(&mut e)?;
    c.set_nz(v, size);
    Ok(())
}

fn op_tas(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_B)?;
    let v = c.ea_read(&mut e)?;
    c.set_nz(v, SZ_B);
    if e.kind == EaKind::Mem {
        c.cycles(2);
    }
    c.ea_write(&mut e, v | 0x80)
}

// ---- Table -------------------------------------------------------------------

/// MOVE: source in bits 5-0, destination register/mode in bits 11-6.
fn fill_move(tb: &mut OpTable, base: u16, src_classes: u32, h: Handler, ha: Option<Handler>) {
    for dst in 0..64u16 {
        let (dmode, dreg) = (u32::from(dst & 7), u32::from(dst >> 3));
        let f = if ea_valid(dmode, dreg, EA_DATA_ALT) {
            h
        } else if let (1, Some(ha)) = (dmode, ha) {
            ha
        } else {
            continue;
        };
        tb.fill(base | dst << 6, 0x003F, src_classes, f);
    }
}

pub fn fill(tb: &mut OpTable) {
    fill_move(tb, 0x1000, EA_DATA, op_move::<SZ_B>, None);
    fill_move(tb, 0x3000, EA_ALL, op_move::<SZ_W>, Some(op_movea_w));
    fill_move(tb, 0x2000, EA_ALL, op_move::<SZ_L>, Some(op_movea_l));
    tb.fill(0x7000, 0x0EFF, 0, op_moveq);

    tb.fill(
        0x4880,
        0x003F,
        EA_CONTROL_ALT | EAB_PREDEC,
        op_movem_to_mem::<SZ_W>,
    );
    tb.fill(
        0x48C0,
        0x003F,
        EA_CONTROL_ALT | EAB_PREDEC,
        op_movem_to_mem::<SZ_L>,
    );
    tb.fill(
        0x4C80,
        0x003F,
        EA_CONTROL | EAB_POSTINC,
        op_movem_to_regs::<SZ_W>,
    );
    tb.fill(
        0x4CC0,
        0x003F,
        EA_CONTROL | EAB_POSTINC,
        op_movem_to_regs::<SZ_L>,
    );
    tb.fill(0x0108, 0x0EC7, 0, op_movep);

    tb.fill(0x41C0, 0x0E3F, EA_CONTROL, op_lea);
    tb.fill(0x4840, 0x003F, EA_CONTROL, op_pea);
    tb.fill(0x4E50, 0x0007, 0, op_link);
    tb.fill(0x4E58, 0x0007, 0, op_unlk);

    tb.fill(0xC140, 0x0E07, 0, op_exg);
    tb.fill(0xC148, 0x0E07, 0, op_exg);
    tb.fill(0xC188, 0x0E07, 0, op_exg);
    tb.fill(0x4840, 0x0007, 0, op_swap);
    tb.fill(0x4880, 0x0007, 0, op_ext_w);
    tb.fill(0x48C0, 0x0007, 0, op_ext_l);

    for size in 0..3u16 {
        tb.fill(0x4200 | size << 6, 0x003F, EA_DATA_ALT, op_clr);
        tb.fill(0x4A00 | size << 6, 0x003F, EA_DATA_ALT, op_tst);
    }
    tb.fill(0x4AC0, 0x003F, EA_DATA_ALT, op_tas);
}
