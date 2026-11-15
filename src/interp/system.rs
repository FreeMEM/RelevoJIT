// SPDX-License-Identifier: MPL-2.0
//! System control: the status register and CCR, USP, STOP and RESET.

use super::{EA_DATA, EA_DATA_ALT, EaKind, OpTable, SZ_W, reg0};
use crate::cpu::{Cpu, Res, Status};

/// MOVE from SR: not privileged on the 68000, where it reads the operand
/// first, like CLR. (Privileged from the 68010 on: RR2.)
fn op_move_from_sr(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    c.ea_read(&mut e)?;
    if e.kind == EaKind::DReg {
        c.cycles(2);
    }
    c.ea_write(&mut e, u32::from(c.r.sr))
}

fn op_move_to_ccr(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let v = c.ea_read(&mut e)?;
    c.set_ccr_bits(0x1F, v);
    c.cycles(8);
    Ok(())
}

fn op_move_to_sr(c: &mut Cpu, op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let v = c.ea_read(&mut e)?;
    c.set_sr(v as u16);
    c.cycles(8);
    Ok(())
}

/// MOVE An,USP and MOVE USP,An. a[7] is the SSP here (supervisor mode).
fn op_move_usp(c: &mut Cpu, op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    if op & 8 != 0 {
        c.r.a[reg0(op)] = c.r.usp;
    } else {
        c.r.usp = c.r.a[reg0(op)];
    }
    Ok(())
}

// ANDI, ORI and EORI to CCR and SR: 20 cycles, 12 of them internal (the
// 68000 re-reads the next word, too).

fn op_ori_ccr(c: &mut Cpu, _op: u16) -> Res {
    let v = c.fetch16()?;
    c.r.sr |= (v & 0x1F) as u16;
    c.cycles(12);
    Ok(())
}

fn op_andi_ccr(c: &mut Cpu, _op: u16) -> Res {
    let v = c.fetch16()?;
    c.r.sr &= (v | 0xFF00) as u16;
    c.cycles(12);
    Ok(())
}

fn op_eori_ccr(c: &mut Cpu, _op: u16) -> Res {
    let v = c.fetch16()?;
    c.r.sr ^= (v & 0x1F) as u16;
    c.cycles(12);
    Ok(())
}

fn op_ori_sr(c: &mut Cpu, _op: u16) -> Res {
    if c.supervisor_or_trap()? {
        let v = c.fetch16()?;
        c.set_sr(c.r.sr | v as u16);
        c.cycles(12);
    }
    Ok(())
}

fn op_andi_sr(c: &mut Cpu, _op: u16) -> Res {
    if c.supervisor_or_trap()? {
        let v = c.fetch16()?;
        c.set_sr(c.r.sr & v as u16);
        c.cycles(12);
    }
    Ok(())
}

fn op_eori_sr(c: &mut Cpu, _op: u16) -> Res {
    if c.supervisor_or_trap()? {
        let v = c.fetch16()?;
        c.set_sr(c.r.sr ^ v as u16);
        c.cycles(12);
    }
    Ok(())
}

/// STOP #imm: loads SR and waits for an interrupt (or a trace).
fn op_stop(c: &mut Cpu, _op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let v = c.fetch16()?;
    c.set_sr(v as u16);
    c.cycles = c.cycles.wrapping_sub(4); // the operand was in the queue: no refill
    c.stopped = true;
    c.exit = Status::Stopped;
    c.exiting = true;
    Ok(())
}

/// RESET: resets the devices, not the CPU.
fn op_reset(c: &mut Cpu, _op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    c.cycles(128);
    c.host_reset();
    Ok(())
}

pub fn fill(tb: &mut OpTable) {
    tb.fill(0x40C0, 0x003F, EA_DATA_ALT, op_move_from_sr);
    tb.fill(0x44C0, 0x003F, EA_DATA, op_move_to_ccr);
    tb.fill(0x46C0, 0x003F, EA_DATA, op_move_to_sr);
    tb.fill(0x4E60, 0x000F, 0, op_move_usp);
    tb.set(0x003C, op_ori_ccr);
    tb.set(0x007C, op_ori_sr);
    tb.set(0x023C, op_andi_ccr);
    tb.set(0x027C, op_andi_sr);
    tb.set(0x0A3C, op_eori_ccr);
    tb.set(0x0A7C, op_eori_sr);
    tb.set(0x4E72, op_stop);
    tb.set(0x4E70, op_reset);
}
