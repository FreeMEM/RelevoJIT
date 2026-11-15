// SPDX-License-Identifier: MPL-2.0
//! Program control: the illegal and unimplemented opcodes, NOP, branches,
//! jumps, returns, TRAP, TRAPV and CHK.

use super::{
    CCR_N, CCR_V, CCR_Z, EA_CONTROL, EA_DATA, EA_DATA_ALT, EaKind, OpTable, SZ_B, SZ_L, SZ_W,
    cond_true, reg0, reg9, sext,
};
use crate::capi::TrapResult;
use crate::cpu::{
    Cpu, Res, Status, VEC_CHK, VEC_ILLEGAL, VEC_LINE_A, VEC_LINE_F, VEC_TRAP, VEC_TRAPV,
};

pub fn op_illegal(c: &mut Cpu, _op: u16) -> Res {
    c.exception_group1(VEC_ILLEGAL)
}

/// Line A and line F: the host gets the first say (native calls), then the
/// exception, which stacks the address of the opcode itself.
fn line_trap(c: &mut Cpu, op: u16, line_f: bool) -> Res {
    match c.host_line(line_f, op) {
        TrapResult::Handled => Ok(()),
        TrapResult::Stop => {
            c.exit = Status::HostStop;
            c.exiting = true;
            Ok(())
        }
        TrapResult::Exception => c.exception_group1(if line_f { VEC_LINE_F } else { VEC_LINE_A }),
    }
}

pub fn op_line_a(c: &mut Cpu, op: u16) -> Res {
    line_trap(c, op, false)
}

pub fn op_line_f(c: &mut Cpu, op: u16) -> Res {
    line_trap(c, op, true)
}

fn op_nop(_c: &mut Cpu, _op: u16) -> Res {
    Ok(())
}

/// Bcc, BRA, BSR: 8-bit displacement in the opcode, or 16 bits in the
/// extension word when it is 0.
#[inline(always)]
fn branch_target(c: &mut Cpu, op: u16) -> Res<u32> {
    let base = c.insn_pc.wrapping_add(2);
    let disp = u32::from(op & 0xFF);
    if disp == 0 {
        return Ok(base.wrapping_add(sext(c.fetch16()?, SZ_W)));
    }
    Ok(base.wrapping_add(sext(disp, SZ_B)))
}

fn op_bcc(c: &mut Cpu, op: u16) -> Res {
    if cond_true(c.r.sr, u32::from(op >> 8)) {
        let target = branch_target(c, op)?;
        c.cycles(2);
        return c.jump(target, c.insn_pc.wrapping_add(2));
    }
    c.cycles(4);
    if op & 0xFF == 0 {
        c.r.pc = c.r.pc.wrapping_add(2);
        c.cycles(4); // the skipped word is still prefetched
    }
    Ok(())
}

fn op_bsr(c: &mut Cpu, op: u16) -> Res {
    let target = branch_target(c, op)?;
    c.cycles(2);
    c.push32(c.r.pc)?;
    c.jump(target, target)
}

/// DBcc Dn,<disp>: the counter is written only once the branch is safe.
fn op_dbcc(c: &mut Cpu, op: u16) -> Res {
    let target = c
        .insn_pc
        .wrapping_add(2)
        .wrapping_add(sext(c.fetch16()?, SZ_W));
    if cond_true(c.r.sr, u32::from(op >> 8)) {
        c.cycles(4);
        return Ok(());
    }
    let n = reg0(op);
    let count = c.r.d[n].wrapping_sub(1) & 0xFFFF;
    if count != 0xFFFF {
        c.cycles(2);
        c.jump(target, c.r.pc)?;
    } else {
        c.cycles(6);
    }
    c.set_dreg(n, count, SZ_W);
    Ok(())
}

/// Scc <ea>: the 68000 reads the byte before writing it.
fn op_scc(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_B)?;
    c.ea_read(&mut e)?;
    let t = cond_true(c.r.sr, u32::from(op >> 8));
    if t && e.kind == EaKind::DReg {
        c.cycles(2);
    }
    c.ea_write(&mut e, if t { 0xFF } else { 0 })
}

/// JMP and JSR take 2 cycles more for (d16,An), (d16,PC) and (xxx).W, and
/// 2 more again for the indexed modes.
fn jump_ea_cycles(c: &mut Cpu, mode: u32, reg: u32) {
    if mode == 5 || (mode == 7 && (reg == 0 || reg == 2)) {
        c.cycles(2);
    } else if mode == 6 || (mode == 7 && reg == 3) {
        c.cycles(4);
    }
}

fn op_jmp(c: &mut Cpu, op: u16) -> Res {
    let e = c.ea_decode_op(op, SZ_L)?;
    jump_ea_cycles(c, e.mode, e.reg);
    c.jump(e.addr, c.insn_pc.wrapping_add(2))
}

/// JSR: the return address goes on the stack only after the target is
/// known to be good.
fn op_jsr(c: &mut Cpu, op: u16) -> Res {
    let e = c.ea_decode_op(op, SZ_L)?;
    jump_ea_cycles(c, e.mode, e.reg);
    let ret = c.r.pc;
    if e.addr & 1 != 0 {
        c.jump(e.addr, ret)?;
    }
    c.push32(ret)?;
    c.jump(e.addr, ret)
}

fn op_rts(c: &mut Cpu, _op: u16) -> Res {
    let target = c.pop32()?;
    c.jump(target, c.insn_pc.wrapping_add(2))
}

fn op_rtr(c: &mut Cpu, _op: u16) -> Res {
    let ccr = c.pop16()?;
    let target = c.pop32()?;
    c.set_ccr_bits(0x1F, ccr);
    c.jump(target, c.insn_pc.wrapping_add(2))
}

/// RTE on the 68000: the frame is SR and PC. (The 68010 and later read a
/// format word: RR2.)
fn op_rte(c: &mut Cpu, _op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let sr = c.pop16()?;
    let target = c.pop32()?;
    c.set_sr(sr as u16);
    c.jump(target, c.insn_pc.wrapping_add(2))
}

fn op_trap(c: &mut Cpu, op: u16) -> Res {
    c.exception(VEC_TRAP + u32::from(op & 15), c.r.pc)
}

fn op_trapv(c: &mut Cpu, _op: u16) -> Res {
    if c.r.sr & CCR_V as u16 != 0 {
        return c.exception(VEC_TRAPV, c.r.pc);
    }
    Ok(())
}

/// CHK.W <ea>,Dn: trap if Dn < 0 (N set) or Dn > bound (N clear).
fn op_chk(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    let bound = sext(c.ea_read(&mut e)?, SZ_W) as i32;
    let v = sext(c.r.d[reg9(op)], SZ_W) as i32;
    let mut f = 0;
    if v == 0 {
        f |= CCR_Z;
    }
    let nzvc = CCR_N | CCR_Z | CCR_V | super::CCR_C;
    if v < 0 {
        c.set_ccr_bits(nzvc, f | CCR_N);
        // 2 cycles more unless Dn - bound, in 16 bits, looks positive
        let diff = v.wrapping_sub(bound) as u32 & 0xFFFF;
        c.cycles(if diff != 0 && diff & 0x8000 == 0 {
            4
        } else {
            6
        });
        c.exception(VEC_CHK, c.r.pc)
    } else if v > bound {
        c.set_ccr_bits(nzvc, f);
        c.cycles(4);
        c.exception(VEC_CHK, c.r.pc)
    } else {
        c.set_ccr_bits(nzvc, f);
        c.cycles(6);
        Ok(())
    }
}

pub fn fill(tb: &mut OpTable) {
    tb.set(0x4E71, op_nop);
    tb.set(0x4E73, op_rte);
    tb.set(0x4E75, op_rts);
    tb.set(0x4E76, op_trapv);
    tb.set(0x4E77, op_rtr);
    tb.fill(0x4E40, 0x000F, 0, op_trap);
    tb.fill(0x4EC0, 0x003F, EA_CONTROL, op_jmp);
    tb.fill(0x4E80, 0x003F, EA_CONTROL, op_jsr);
    for cc in 0..16u16 {
        // BRA, Bcc
        if cc != 1 {
            tb.fill(0x6000 | cc << 8, 0x00FF, 0, op_bcc);
        }
    }
    tb.fill(0x6100, 0x00FF, 0, op_bsr);
    tb.fill(0x50C8, 0x0F07, 0, op_dbcc);
    tb.fill(0x50C0, 0x0F3F, EA_DATA_ALT, op_scc);
    tb.fill(0x4180, 0x0E3F, EA_DATA, op_chk);
}
