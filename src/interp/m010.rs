// SPDX-License-Identifier: MPL-2.0
//! What the 68010 added, also on the later models: MOVEC, MOVES, RTD and
//! MOVE from CCR. (MOVE from SR becoming privileged is in system.rs; the
//! new exception frames and RTE are in exception.rs and control.rs.)

use super::{EA_DATA_ALT, EA_MEM_ALT, OpTable, SZ_W, sext, size_field};
use crate::cpu::{Cpu, Model, Res, SR_M, SR_S, VEC_ILLEGAL};

// MOVEC control register codes (MC68020UM, MC68040UM, MC68060UM).
const CR_SFC: u32 = 0x000;
const CR_DFC: u32 = 0x001;
const CR_CACR: u32 = 0x002;
const CR_TC: u32 = 0x003;
const CR_ITT0: u32 = 0x004;
const CR_ITT1: u32 = 0x005;
const CR_DTT0: u32 = 0x006;
const CR_DTT1: u32 = 0x007;
const CR_BUSCR: u32 = 0x008;
const CR_USP: u32 = 0x800;
const CR_VBR: u32 = 0x801;
const CR_CAAR: u32 = 0x802;
const CR_MSP: u32 = 0x803;
const CR_ISP: u32 = 0x804;
const CR_MMUSR: u32 = 0x805;
const CR_URP: u32 = 0x806;
const CR_SRP: u32 = 0x807;
const CR_PCR: u32 = 0x808;

/// Whether the model has control register `cr`.
fn creg_exists(m: Model, cr: u32) -> bool {
    match cr {
        CR_SFC | CR_DFC | CR_USP | CR_VBR => true,
        CR_CACR => m >= Model::M68020,
        CR_CAAR => m == Model::M68020 || m == Model::M68030,
        CR_MSP | CR_ISP => m >= Model::M68020 && m <= Model::M68040,
        CR_TC | CR_ITT0 | CR_ITT1 | CR_DTT0 | CR_DTT1 | CR_URP | CR_SRP => m >= Model::M68040,
        CR_MMUSR => m == Model::M68040,
        CR_BUSCR | CR_PCR => m == Model::M68060,
        _ => false,
    }
}

// Bits that hold a value (MC68040UM 3.1 and 4.1; MC68060UM 4.1, 6.1).
const TC_MASK: u32 = 0x0000_C000; // E, P
const TT_MASK: u32 = 0xFFFF_E364; // base, mask, E, S, U1-U0, CM, W
const RP_MASK: u32 = 0xFFFF_FE00; // 512-byte aligned table
const BUSCR_MASK: u32 = 0xF000_0000;
const PCR_MASK: u32 = 0x0000_0083; // EDEBUG, DFP, ESS
const PCR_ID: u32 = 0x0430_0000; // 68060, revision 0

impl Cpu {
    /// The stack pointer that M and S select, wherever it lives now: a[7]
    /// when it is the active one.
    fn stack_reg(&mut self, cr: u32) -> &mut u32 {
        let s = self.r.sr & SR_S != 0;
        let m = self.r.sr & SR_M != 0;
        match cr {
            CR_USP => {
                if s {
                    &mut self.r.usp
                } else {
                    &mut self.r.a[7]
                }
            }
            CR_MSP => {
                if s && m {
                    &mut self.r.a[7]
                } else {
                    &mut self.r.msp
                }
            }
            _ => {
                // ISP
                if s && !m {
                    &mut self.r.a[7]
                } else {
                    &mut self.r.isp
                }
            }
        }
    }

    fn creg_read(&mut self, cr: u32) -> u32 {
        match cr {
            CR_SFC => self.r.sfc,
            CR_DFC => self.r.dfc,
            CR_CACR => self.r.cacr,
            CR_TC => self.r.tc,
            CR_ITT0 => self.r.itt0,
            CR_ITT1 => self.r.itt1,
            CR_DTT0 => self.r.dtt0,
            CR_DTT1 => self.r.dtt1,
            CR_BUSCR => self.r.buscr,
            CR_VBR => self.r.vbr,
            CR_CAAR => self.r.caar,
            CR_MMUSR => self.r.mmusr,
            CR_URP => self.r.urp,
            CR_SRP => self.r.srp,
            CR_PCR => PCR_ID | self.r.pcr,
            _ => *self.stack_reg(cr),
        }
    }

    fn creg_write(&mut self, cr: u32, v: u32) {
        match cr {
            CR_SFC => self.r.sfc = v & 7,
            CR_DFC => self.r.dfc = v & 7,
            CR_CACR => self.write_cacr(v),
            CR_TC => self.r.tc = v & TC_MASK,
            CR_ITT0 => self.r.itt0 = v & TT_MASK,
            CR_ITT1 => self.r.itt1 = v & TT_MASK,
            CR_DTT0 => self.r.dtt0 = v & TT_MASK,
            CR_DTT1 => self.r.dtt1 = v & TT_MASK,
            CR_BUSCR => self.r.buscr = v & BUSCR_MASK,
            CR_VBR => self.r.vbr = v,
            CR_CAAR => self.r.caar = v,
            CR_MMUSR => self.r.mmusr = v, // every bit, as FS-UAE
            CR_URP => self.r.urp = v & RP_MASK,
            CR_SRP => self.r.srp = v & RP_MASK,
            CR_PCR => self.r.pcr = v & PCR_MASK,
            _ => *self.stack_reg(cr) = v,
        }
    }
}

/// MOVEC Rc,Rn (0x4E7A) and MOVEC Rn,Rc (0x4E7B). A register the model
/// lacks is an illegal instruction.
fn op_movec(c: &mut Cpu, op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let ext = c.fetch16()?;
    let cr = ext & 0x0FFF;
    if !creg_exists(c.model, cr) {
        return c.exception_group1(VEC_ILLEGAL);
    }
    let n = ((ext >> 12) & 7) as usize;
    let areg = ext & 0x8000 != 0;
    if op & 1 != 0 {
        let v = if areg { c.r.a[n] } else { c.r.d[n] };
        c.creg_write(cr, v);
    } else {
        let v = c.creg_read(cr);
        if areg {
            c.r.a[n] = v;
        } else {
            c.r.d[n] = v;
        }
    }
    c.cycles(6);
    Ok(())
}

/// MOVES <ea>,Rn and MOVES Rn,<ea>: an access with the function code in
/// SFC or DFC. RelevoJIT has no function codes on its bus, so it is an
/// ordinary access. Into An, the operand is sign-extended.
fn op_moves(c: &mut Cpu, op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let ext = c.fetch16()?;
    let size = size_field(op);
    let n = ((ext >> 12) & 7) as usize;
    let areg = ext & 0x8000 != 0;
    if ext & 0x0800 != 0 {
        let v = if areg { c.r.a[n] } else { c.r.d[n] };
        let mut e = c.ea_decode_op(op, size)?;
        c.ea_write(&mut e, v)?;
    } else {
        let mut e = c.ea_decode_op(op, size)?;
        let v = c.ea_read(&mut e)?;
        if areg {
            c.r.a[n] = sext(v, size);
        } else {
            c.set_dreg(n, v, size);
        }
    }
    c.cycles(4);
    Ok(())
}

/// RTD #d16: return and deallocate.
fn op_rtd(c: &mut Cpu, _op: u16) -> Res {
    let disp = sext(c.fetch16()?, SZ_W);
    let target = c.pop32()?;
    c.r.a[7] = c.r.a[7].wrapping_add(disp);
    c.jump(target, c.insn_pc.wrapping_add(2))
}

/// MOVE CCR,<ea>: a word, the CCR in the low byte.
fn op_move_from_ccr(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_W)?;
    c.ea_write(&mut e, u32::from(c.r.sr & 0x1F))
}

pub fn fill(tb: &mut OpTable) {
    if tb.model < Model::M68010 {
        return;
    }
    tb.set(0x4E7A, op_movec);
    tb.set(0x4E7B, op_movec);
    tb.set(0x4E74, op_rtd);
    tb.fill(0x42C0, 0x003F, EA_DATA_ALT, op_move_from_ccr);
    for s in 0..3u16 {
        tb.fill(0x0E00 | s << 6, 0x003F, EA_MEM_ALT, op_moves);
    }
}
