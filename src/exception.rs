// SPDX-License-Identifier: MPL-2.0
//! Exception processing: groups 1 and 2 (the 68000's short frame), group 0
//! (address and bus errors, the 68000's long frame), trace and interrupts.
//!
//! Group 0 exceptions abandon the instruction half done. They are raised
//! from inside memory accesses, stack their frame and return [`Fault`],
//! which every handler passes up to the run loop; the loop goes on with the
//! next instruction (or returns `Halted` after a double bus fault).
//!
//! The frames of the 68010 and later models come with them (RR2).

use core::sync::atomic::Ordering::Relaxed;

use crate::cpu::{
    ACC_DATA, ACC_PROG, ACC_READ, Cpu, Fault, Model, Res, SR_IPL, SR_S, SR_T0, SR_T1,
    VEC_ADDRESS_ERROR, VEC_AUTOVECTOR, VEC_BUS_ERROR, VEC_SPURIOUS, VEC_TRACE, VEC_TRAP,
};

impl Cpu {
    /// Pushes on the active stack (a[7]) with the CPU's own accesses.
    pub fn push16(&mut self, v: u32) -> Res {
        self.r.a[7] = self.r.a[7].wrapping_sub(2);
        self.write16(self.r.a[7], v)
    }

    pub fn push32(&mut self, v: u32) -> Res {
        self.r.a[7] = self.r.a[7].wrapping_sub(4);
        self.write32(self.r.a[7], v)
    }

    fn vector_base(&self) -> u32 {
        if self.model == Model::M68000 {
            0
        } else {
            self.r.vbr
        }
    }

    /// Supervisor mode, trace off; returns the SR to stack.
    fn enter_supervisor(&mut self) -> u16 {
        let old_sr = self.r.sr;
        self.set_sr((old_sr | SR_S) & !(SR_T1 | SR_T0));
        old_sr
    }

    fn take_vector(&mut self, vector: u32) -> Res {
        let pc = self.read32(self.vector_base().wrapping_add(vector * 4), ACC_DATA)?;
        self.stopped = false;
        if pc & 1 != 0 {
            // The first prefetch of the handler fails.
            self.ae_pc = pc;
            return Err(self.address_error(pc, ACC_PROG | ACC_READ));
        }
        self.r.pc = pc;
        Ok(())
    }

    /// Cycles: an exception refills the prefetch queue from the handler (two
    /// words). Between instructions both count; inside one, the
    /// instruction's first word, counted when fetched, stands for one of
    /// them.
    fn refill_cycles(&self) -> u64 {
        if self.in_insn { 4 } else { 8 }
    }

    /// Group 1 and 2 exceptions: the 68000 frame (PC, SR) with `pc` as the
    /// return address, then the jump through the vector.
    pub fn exception(&mut self, vector: u32, pc: u32) -> Res {
        let irq = vector > VEC_SPURIOUS && vector < VEC_TRAP;
        self.cycles += self.refill_cycles() + if irq { 16 } else { 6 };
        let old_sr = self.enter_supervisor();
        self.push32(pc)?;
        self.push16(u32::from(old_sr))?;
        self.take_vector(vector)
    }

    /// Illegal, privilege violation, line A and line F: the instruction does
    /// not run, its own address is stacked, and it is not traced.
    pub fn exception_group1(&mut self, vector: u32) -> Res {
        self.trace_pending = false;
        self.exception(vector, self.insn_pc)
    }

    #[cold]
    #[inline(never)]
    fn group0(&mut self, vector: u32, addr: u32, acc: u32) -> Fault {
        if self.in_group0 {
            // A fault while stacking a fault: the 68000 halts.
            self.in_group0 = false;
            self.halted = true;
            return Fault;
        }
        self.in_group0 = true;
        self.trace_pending = false;
        // Internal cycles and the two prefetches from the handler. The words
        // of the instruction counted so far stand for the faulted access.
        self.cycles = self
            .cycles
            .wrapping_add_signed(i64::from(18 + self.fault_cycles));

        // Special status word: the upper bits of IR (undocumented, as the
        // MAME microcode does), R/W, I/N (0) and the function code.
        let old_sr = self.r.sr;
        let ssw = (u32::from(self.ir) & 0xFFE0)
            | (acc & ACC_READ)
            | if old_sr & SR_S != 0 { 4 } else { 0 }
            | (acc & (ACC_DATA | ACC_PROG));
        self.enter_supervisor();
        if self.group0_frame(vector, addr, ssw, old_sr).is_ok() {
            self.in_group0 = false;
        }
        Fault
    }

    /// The 68000's long frame: PC, SR, IR, the access address and the
    /// special status word; then the vector.
    fn group0_frame(&mut self, vector: u32, addr: u32, ssw: u32, old_sr: u16) -> Res {
        self.push32(self.ae_pc)?;
        self.push16(u32::from(old_sr))?;
        self.push16(u32::from(self.ir))?;
        self.push32(addr)?;
        self.push16(ssw)?;
        self.take_vector(vector)
    }

    /// An address error: returns the [`Fault`] that ends the instruction.
    #[cold]
    pub fn address_error(&mut self, addr: u32, acc: u32) -> Fault {
        self.fault_size = 2;
        self.group0(VEC_ADDRESS_ERROR, addr, acc)
    }

    #[cold]
    pub fn bus_error(&mut self, addr: u32, acc: u32) -> Fault {
        self.group0(VEC_BUS_ERROR, addr, acc)
    }

    /// Autovectored interrupt: the new mask is the level taken.
    fn interrupt(&mut self, level: u32) -> Res {
        let pc = self.r.pc;
        self.exception(VEC_AUTOVECTOR + level, pc)?;
        self.r.sr = (self.r.sr & !SR_IPL) | (level << 8) as u16;
        Ok(())
    }

    /// Between instructions: a pending trace, then interrupts. Returns false
    /// if the CPU stays stopped.
    pub fn between_insns(&mut self) -> Res<bool> {
        if self.trace_pending {
            self.trace_pending = false;
            self.exception(VEC_TRACE, self.r.pc)?;
        }
        let ipl = self.shared.ipl.load(Relaxed);
        let mask = u32::from((self.r.sr & SR_IPL) >> 8);
        let nmi = ipl == 7 && self.prev_ipl != 7; // level 7: on the edge
        self.prev_ipl = ipl;
        if ipl > mask || nmi {
            self.interrupt(ipl)?;
        }
        Ok(!self.stopped)
    }
}
