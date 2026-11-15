// SPDX-License-Identifier: MPL-2.0
//! The interpreter: one handler per 16-bit opcode word, and what the
//! handlers share (operand sizes, the instruction stream, condition codes,
//! effective addresses, jumps).
//!
//! Adding instructions
//! -------------------
//! Each group of instructions lives in its own file with one entry point,
//! `fill(tb: &mut OpTable)`, that puts its handlers in the table with
//! [`OpTable::fill`]; [`build_table`] calls them in order.
//!
//! A handler gets the opcode word; `r.pc` already points after it. It reads
//! its extension words with `fetch16()`/`fetch32()` or through
//! `ea_decode()`, in the order they come. Memory goes through
//! `ea_read()`/`ea_write()` or the CPU's accesses (memory.rs), which raise
//! address and bus errors by themselves: they return `Err(Fault)`, which the
//! handler passes up with `?`.
//!
//! Address errors on the 68000: the frame holds the PC the microcode had
//! reached, not the instruction's address. `ea_read()` and `ea_write()` stack
//! the value that `ea_decode()` worked out (`Ea::ae_pc`); handlers that write
//! after the next prefetch (MOVE, MOVEM) set it themselves. See ea.rs.
//!
//! Cycles: bus accesses count themselves (4 per byte or word, every word of
//! the instruction included), `ea_decode()`/`ea_read()` add the 2 cycles of
//! the indexed modes and of -(An) on reads, `jump()` and the exceptions add
//! the prefetches from the new address. A handler adds the rest with
//! `cycles()`: the manual's time for the instruction minus 4 per word and per
//! access. SingleStepTests checks it ("cyc" column of build/rust/sst).

use alloc::boxed::Box;
use alloc::vec;

use crate::cpu::{
    ACC_DATA, ACC_PROG, Cpu, Model, Res, SR_C, SR_N, SR_S, SR_V, SR_Z, VEC_PRIVILEGE,
};

mod arith;
mod bits;
mod control;
mod ea;
mod moves;
mod muldiv;
mod system;

pub use ea::{Ea, EaKind};

/// One handler per opcode. The opcode word is already fetched and `r.pc`
/// points after it.
pub type Handler = fn(&mut Cpu, u16) -> Res;

// ---- Sizes -------------------------------------------------------------------

pub const SZ_B: u32 = 1;
pub const SZ_W: u32 = 2;
pub const SZ_L: u32 = 4;

#[inline(always)]
pub fn sz_mask(size: u32) -> u32 {
    if size == SZ_L {
        0xFFFF_FFFF
    } else {
        (1 << (8 * size)) - 1
    }
}

#[inline(always)]
pub fn sz_msb(size: u32) -> u32 {
    1 << (8 * size - 1)
}

/// Sign extension of the low `size` bytes to 32 bits.
#[inline(always)]
pub fn sext(v: u32, size: u32) -> u32 {
    let m = sz_msb(size);
    ((v & sz_mask(size)) ^ m).wrapping_sub(m)
}

/// The usual size field (bits 7-6): 00 byte, 01 word, 10 long; 0 for 11.
#[inline(always)]
pub fn size_field(op: u16) -> u32 {
    const SIZES: [u32; 4] = [SZ_B, SZ_W, SZ_L, 0];
    SIZES[usize::from((op >> 6) & 3)]
}

/// Bits 11-9 of the opcode, as an index (register number).
#[inline(always)]
pub fn reg9(op: u16) -> usize {
    usize::from((op >> 9) & 7)
}

/// Bits 2-0 of the opcode, as an index (register number).
#[inline(always)]
pub fn reg0(op: u16) -> usize {
    usize::from(op & 7)
}

// ---- Condition codes -----------------------------------------------------

pub const CCR_C: u32 = SR_C as u32;
pub const CCR_V: u32 = SR_V as u32;
pub const CCR_Z: u32 = SR_Z as u32;
pub const CCR_N: u32 = SR_N as u32;
pub const CCR_X: u32 = crate::cpu::SR_X as u32;

/// Condition `cc` (0-15: T F HI LS CC CS NE EQ VC VS PL MI GE LT GT LE).
#[inline(always)]
pub fn cond_true(sr: u16, cc: u32) -> bool {
    let c = sr & SR_C != 0;
    let v = sr & SR_V != 0;
    let z = sr & SR_Z != 0;
    let n = sr & SR_N != 0;
    match cc & 15 {
        0 => true,
        1 => false,
        2 => !c && !z,
        3 => c || z,
        4 => !c,
        5 => c,
        6 => !z,
        7 => z,
        8 => !v,
        9 => v,
        10 => !n,
        11 => n,
        12 => n == v,
        13 => n != v,
        14 => !z && n == v,
        _ => z || n != v,
    }
}

impl Cpu {
    /// Writes the low `size` bytes of a data register.
    #[inline(always)]
    pub fn set_dreg(&mut self, reg: usize, v: u32, size: u32) {
        let m = sz_mask(size);
        self.r.d[reg & 7] = (self.r.d[reg & 7] & !m) | (v & m);
    }

    // ---- Instruction stream ----

    #[inline(always)]
    pub fn fetch16(&mut self) -> Res<u32> {
        let v = self.read16(self.r.pc, ACC_PROG)?;
        self.r.pc = self.r.pc.wrapping_add(2);
        Ok(v)
    }

    #[inline(always)]
    pub fn fetch32(&mut self) -> Res<u32> {
        let v = self.read32(self.r.pc, ACC_PROG)?;
        self.r.pc = self.r.pc.wrapping_add(4);
        Ok(v)
    }

    // ---- Memory by size ----

    #[inline(always)]
    pub fn read(&mut self, addr: u32, size: u32, acc: u32) -> Res<u32> {
        match size {
            SZ_B => self.read8(addr, acc),
            SZ_W => self.read16(addr, acc),
            _ => self.read32(addr, acc),
        }
    }

    #[inline(always)]
    pub fn write(&mut self, addr: u32, v: u32, size: u32) -> Res {
        match size {
            SZ_B => self.write8(addr, v),
            SZ_W => self.write16(addr, v),
            _ => self.write32(addr, v),
        }
    }

    pub fn pop16(&mut self) -> Res<u32> {
        let v = self.read16(self.r.a[7], ACC_DATA)?;
        self.r.a[7] = self.r.a[7].wrapping_add(2);
        Ok(v)
    }

    pub fn pop32(&mut self) -> Res<u32> {
        let v = self.read32(self.r.a[7], ACC_DATA)?;
        self.r.a[7] = self.r.a[7].wrapping_add(4);
        Ok(v)
    }

    // ---- Cycles and flags ----

    /// Internal cycles of an instruction, beyond its bus accesses.
    #[inline(always)]
    pub fn cycles(&mut self, n: u32) {
        self.cycles += u64::from(n);
    }

    /// Replaces the bits in `mask` of the CCR with those of `bits`.
    #[inline(always)]
    pub fn set_ccr_bits(&mut self, mask: u32, bits: u32) {
        self.r.sr = (self.r.sr & !(mask as u16)) | (bits & mask) as u16;
    }

    /// N and Z from the result; V and C cleared; X kept. The flags of MOVE,
    /// CLR, TST, the logical operations...
    #[inline(always)]
    pub fn set_nz(&mut self, v: u32, size: u32) {
        let v = v & sz_mask(size);
        let mut f = 0;
        if v == 0 {
            f |= CCR_Z;
        }
        if v & sz_msb(size) != 0 {
            f |= CCR_N;
        }
        self.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, f);
    }

    // ---- Flow ----

    /// PC = target. An odd target is an address error when the 68000
    /// prefetches there, still inside the jumping instruction; `ae_pc` is
    /// what it stacks. Counts the cycles of refilling the prefetch queue at
    /// the target.
    pub fn jump(&mut self, target: u32, ae_pc: u32) -> Res {
        // The queue gets two words from the target. The instruction's words,
        // counted when fetched, stand for the 68000's reads, except that it
        // never refills behind the last extension word of a jump.
        let words = self.r.pc.wrapping_sub(self.insn_pc) / 2;
        if target & 1 != 0 {
            if words >= 2 {
                self.cycles -= 4;
            }
            self.ae_pc = ae_pc;
            return Err(self.address_error(target, ACC_PROG | crate::cpu::ACC_READ));
        }
        if words < 2 {
            self.cycles(8 - 4 * words);
        }
        self.r.pc = target;
        Ok(())
    }

    /// Privileged instructions: false (after taking the privilege
    /// violation) in user mode.
    #[inline(always)]
    pub fn supervisor_or_trap(&mut self) -> Res<bool> {
        if self.r.sr & SR_S != 0 {
            return Ok(true);
        }
        self.exception_group1(VEC_PRIVILEGE)?;
        Ok(false)
    }
}

// ---- The opcode table ------------------------------------------------------

// Effective address classes, as sets of mode/reg slots (M68000PRM 2.2).
pub const EAB_DREG: u32 = 1 << 0;
pub const EAB_AREG: u32 = 1 << 1;
pub const EAB_IND: u32 = 1 << 2; // (An)
pub const EAB_POSTINC: u32 = 1 << 3; // (An)+
pub const EAB_PREDEC: u32 = 1 << 4; // -(An)
pub const EAB_D16: u32 = 1 << 5; // (d16,An)
pub const EAB_D8: u32 = 1 << 6; // (d8,An,Xn)
pub const EAB_ABSW: u32 = 1 << 7; // (xxx).W
pub const EAB_ABSL: u32 = 1 << 8; // (xxx).L
pub const EAB_PCD16: u32 = 1 << 9; // (d16,PC)
pub const EAB_PCD8: u32 = 1 << 10; // (d8,PC,Xn)
pub const EAB_IMM: u32 = 1 << 11; // #imm

pub const EA_ALL: u32 = 0x0FFF;
pub const EA_DATA: u32 = EA_ALL & !EAB_AREG;
pub const EA_CONTROL: u32 = EAB_IND | EAB_D16 | EAB_D8 | EAB_ABSW | EAB_ABSL | EAB_PCD16 | EAB_PCD8;
pub const EA_ALTERABLE: u32 = EAB_DREG
    | EAB_AREG
    | EAB_IND
    | EAB_POSTINC
    | EAB_PREDEC
    | EAB_D16
    | EAB_D8
    | EAB_ABSW
    | EAB_ABSL;
pub const EA_DATA_ALT: u32 = EA_ALTERABLE & !EAB_AREG;
pub const EA_MEM_ALT: u32 = EA_DATA_ALT & !EAB_DREG;
pub const EA_CONTROL_ALT: u32 = EA_CONTROL & !(EAB_PCD16 | EAB_PCD8);

/// Whether mode/reg belongs to `classes`.
pub fn ea_valid(mode: u32, reg: u32, classes: u32) -> bool {
    if mode == 7 && reg > 4 {
        return false;
    }
    let slot = if mode < 7 { mode } else { 7 + reg };
    (classes >> slot) & 1 != 0
}

pub struct OpTable {
    t: Box<[Handler; 0x10000]>,
    /// Opcodes a group has claimed
    taken: Box<[bool; 0x10000]>,
    /// Opcodes given a handler twice
    conflicts: u32,
}

impl OpTable {
    /// One opcode.
    pub fn set(&mut self, op: u16, h: Handler) {
        let i = usize::from(op);
        if self.taken[i] {
            self.conflicts += 1;
        }
        self.taken[i] = true;
        self.t[i] = h;
    }

    /// Puts `h` at every opcode `base | s`, for every combination s of the
    /// bits in `free`, whose low 6 bits are an effective address in
    /// `ea_classes` (0: no check).
    pub fn fill(&mut self, base: u16, free: u16, ea_classes: u32, h: Handler) {
        // Walks every subset s of `free`.
        let mut s: u16 = 0;
        loop {
            let op = base | s;
            if ea_classes == 0 || ea_valid(u32::from((op >> 3) & 7), u32::from(op & 7), ea_classes)
            {
                self.set(op, h);
            }
            s = s.wrapping_sub(free) & free;
            if s == 0 {
                break;
            }
        }
    }
}

/// The table for `model`, and how many opcodes two groups claimed (0
/// unless there is a bug). Opcodes the model does not execute go to the
/// illegal instruction, line A or line F handlers. Only the 68000 so far:
/// the groups of the 68010 and later models come with RR2.
pub fn build_table(_model: Model) -> (Box<[Handler; 0x10000]>, u32) {
    let mut t: Box<[Handler; 0x10000]> = vec![control::op_illegal as Handler; 0x10000]
        .into_boxed_slice()
        .try_into()
        .expect("0x10000 handlers");
    t[0xA000..=0xAFFF].fill(control::op_line_a);
    t[0xF000..=0xFFFF].fill(control::op_line_f);
    let mut tb = OpTable {
        t,
        taken: vec![false; 0x10000]
            .into_boxed_slice()
            .try_into()
            .expect("0x10000 entries"),
        conflicts: 0,
    };
    control::fill(&mut tb);
    moves::fill(&mut tb);
    system::fill(&mut tb);
    arith::fill(&mut tb);
    bits::fill(&mut tb);
    muldiv::fill(&mut tb);
    (tb.t, tb.conflicts)
}
