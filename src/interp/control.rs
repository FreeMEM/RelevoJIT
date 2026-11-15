// SPDX-License-Identifier: MPL-2.0
//! Program control: the illegal and unimplemented opcodes, NOP, branches,
//! jumps, returns, TRAP, TRAPV and CHK.

use super::{
    CCR_N, CCR_V, CCR_Z, EA_CONTROL, EA_DATA, EA_DATA_ALT, EaKind, OpTable, SZ_B, SZ_L, SZ_W,
    cond_true, reg0, reg9, sext,
};
use crate::capi::TrapResult;
use crate::cpu::{
    ACC_DATA, Cpu, Model, Res, Status, VEC_CHK, VEC_FORMAT_ERROR, VEC_ILLEGAL, VEC_LINE_A,
    VEC_LINE_F, VEC_TRAP, VEC_TRAPV,
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
/// extension word when it is 0 (32 bits when it is 0xFF, 68020 on).
#[inline(always)]
fn branch_target(c: &mut Cpu, op: u16) -> Res<u32> {
    let base = c.insn_pc.wrapping_add(2);
    let disp = u32::from(op & 0xFF);
    if disp == 0 {
        return Ok(base.wrapping_add(sext(c.fetch16()?, SZ_W)));
    }
    if disp == 0xFF && c.model >= Model::M68020 {
        return Ok(base.wrapping_add(c.fetch32()?));
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
    } else if op & 0xFF == 0xFF && c.model >= Model::M68020 {
        c.r.pc = c.r.pc.wrapping_add(4);
        c.cycles(8);
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

/// Bytes of an exception frame of format `fmt` on model `m`, or 0 if RTE
/// does not take that format there (a format error). MC68010 5.5,
/// MC68020UM 6.4, MC68040UM 8.4, MC68060UM 8.5. (FS-UAE takes formats $4,
/// $7, $8, $9, $A and $B on both the 68020 and the 68040, and not $3 on the
/// 68040; RelevoJIT follows the manuals.)
fn frame_bytes(m: Model, fmt: u32) -> u32 {
    let m020_030 = m == Model::M68020 || m == Model::M68030;
    match fmt {
        0x0 => 8,
        0x1 if m >= Model::M68020 && m <= Model::M68040 => 8,
        0x2 if m >= Model::M68020 => 12,
        0x3 if m >= Model::M68040 => 12,
        0x4 if m >= Model::M68040 => 16,
        0x7 if m == Model::M68040 => 60,
        0x8 if m == Model::M68010 => 58,
        0x9 if m020_030 => 20,
        0xA if m020_030 => 32,
        0xB if m020_030 => 92,
        _ => 0,
    }
}

/// RTE. On the 68000 the frame is SR and PC. From the 68010 on, the format
/// word says how big the frame is; a format the model does not have is a
/// format error, taken with the RTE's own address, after the first four
/// words of the bad frame are popped (as FS-UAE, the bench's oracle). A
/// format $1 (throwaway) frame only restores SR, which sets M again: RTE
/// goes on with the frame on the master stack. Frames of faults (formats
/// $7, $8, $A, $B) resume at their PC: RelevoJIT restarts the instruction
/// there instead of finishing the faulted bus cycle.
fn op_rte(c: &mut Cpu, _op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    if c.model == Model::M68000 {
        let sr = c.pop16()?;
        let target = c.pop32()?;
        c.set_sr(sr as u16);
        return c.jump(target, c.insn_pc.wrapping_add(2));
    }
    loop {
        let sp = c.r.a[7];
        let sr = c.read16(sp, ACC_DATA)?;
        let target = c.read32(sp.wrapping_add(2), ACC_DATA)?;
        let fmt = c.read16(sp.wrapping_add(6), ACC_DATA)? >> 12;
        let size = frame_bytes(c.model, fmt);
        if size == 0 {
            c.r.a[7] = sp.wrapping_add(8);
            return c.exception_group1(VEC_FORMAT_ERROR);
        }
        c.r.a[7] = sp.wrapping_add(size);
        c.set_sr(sr as u16);
        if fmt != 1 {
            return c.jump(target, c.insn_pc.wrapping_add(2));
        }
    }
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
    if c.model >= Model::M68020 {
        c.cycles(6);
        return c.chk_020(v, bound);
    }
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
