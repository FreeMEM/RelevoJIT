// SPDX-License-Identifier: MPL-2.0
//! Exception processing: groups 1 and 2 (short frame; format $0, or $2 on
//! the 68020+), group 0 (address and bus errors: the 68000's long frame,
//! format $8 on the 68010, $B on the 68020/030, $2 or $7 on the 68040),
//! trace and interrupts.
//!
//! Group 0 exceptions abandon the instruction half done. They are raised
//! from inside memory accesses, stack their frame and return [`Fault`],
//! which every handler passes up to the run loop; the loop goes on with the
//! next instruction (or returns `Halted` after a double bus fault).

use core::sync::atomic::Ordering::Relaxed;

use crate::cpu::{
    ACC_DATA, ACC_PROG, ACC_READ, Cpu, Fault, Model, Res, SR_IPL, SR_M, SR_S, SR_T0, SR_T1,
    VEC_ADDRESS_ERROR, VEC_AUTOVECTOR, VEC_BUS_ERROR, VEC_CHK, VEC_SPURIOUS, VEC_TRACE, VEC_TRAP,
    VEC_TRAPV, VEC_ZERO_DIVIDE,
};

/// Exceptions that stack format $2 (with the instruction's address) from
/// the 68020 on: zero divide, CHK and CHK2, TRAPV and TRAPcc, trace.
fn format2_vector(vector: u32) -> bool {
    matches!(vector, VEC_ZERO_DIVIDE | VEC_CHK | VEC_TRAPV | VEC_TRACE)
}

/// The function code of a faulted access: data or program, user or
/// supervisor (1, 2, 5, 6).
fn fault_fc(sr: u16, acc: u32) -> u16 {
    ((acc & (ACC_DATA | ACC_PROG)) | if sr & SR_S != 0 { 4 } else { 0 }) as u16
}

/// The size field of the 68020/030 and 68040 special status words: byte 1,
/// word 2, long 0.
fn size_code(size: u32) -> u16 {
    if size == 4 { 0 } else { (size & 3) as u16 }
}

fn put32(w: &mut [u16], v: u32) {
    w[0] = (v >> 16) as u16;
    w[1] = v as u16;
}

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

    /// Group 1 and 2 exceptions, with `pc` as the return address: the 68000
    /// frame (PC, SR), the 68010+ format $0 frame (PC, SR, format/vector
    /// word), or format $2 on the 68020+: the same and an address, that of
    /// the instruction that caused it on the 68020 and 68030, the next one
    /// (the stacked PC) on the 68040 and 68060, as FS-UAE, the bench's
    /// oracle, does. Then the jump through the vector.
    pub fn exception(&mut self, vector: u32, pc: u32) -> Res {
        let irq = vector > VEC_SPURIOUS && vector < VEC_TRAP;
        self.cycles += self.refill_cycles() + if irq { 16 } else { 6 };
        let old_sr = self.enter_supervisor();
        if self.model >= Model::M68020 && format2_vector(vector) {
            let addr = if self.model >= Model::M68040 {
                pc
            } else {
                self.insn_pc
            };
            self.push32(addr)?;
            self.push16(0x2000 | ((vector * 4) & 0x0FFF))?;
        } else if self.model != Model::M68000 {
            self.push16((vector * 4) & 0x0FFF)?; // format 0
        }
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
        if self.group0_frame(vector, addr, acc, ssw, old_sr).is_ok() {
            self.in_group0 = false;
        }
        Fault
    }

    /// The 68000's long frame (PC, SR, IR, the access address and the
    /// special status word), or the frame of a later model; then the vector.
    fn group0_frame(&mut self, vector: u32, addr: u32, acc: u32, ssw: u32, old_sr: u16) -> Res {
        if self.model == Model::M68000 {
            self.push32(self.ae_pc)?;
            self.push16(u32::from(old_sr))?;
            self.push16(u32::from(self.ir))?;
            self.push32(addr)?;
            self.push16(ssw)?;
        } else {
            self.group0_frame_010(vector, addr, acc, old_sr)?;
        }
        self.take_vector(vector)
    }

    /// Pushes a frame given as words from its lowest address up.
    fn push_frame(&mut self, w: &[u16]) -> Res {
        for &v in w.iter().rev() {
            self.push16(u32::from(v))?;
        }
        Ok(())
    }

    /// Address and bus error frames of the 68010 and later. The frame's PC
    /// is the instruction's own address: RTE restarts it (RelevoJIT keeps no
    /// internal state to finish a faulted bus cycle). Internal words are 0.
    fn group0_frame_010(&mut self, vector: u32, addr: u32, acc: u32, sr: u16) -> Res {
        let mut w = [0u16; 46];
        let fc = fault_fc(sr, acc);
        let size = self.fault_size;
        let read = acc & ACC_READ != 0;
        let prog = acc & ACC_PROG != 0;
        let fv = ((vector * 4) & 0x0FFF) as u16;
        w[0] = sr;
        put32(&mut w[1..], self.insn_pc);

        match self.model {
            Model::M68010 => {
                // Format $8 (MC68010 5.5.4): SSW with RR, IF, DF, RM, HB,
                // BY, RW, FC; the fault address; the rest is internal.
                w[3] = 0x8000 | fv;
                w[4] = (if prog { 0x2000 } else { 0x1000 })
                    | (if size == 1 { 0x0200 } else { 0 })
                    | (if read { 0x0100 } else { 0 })
                    | fc;
                put32(&mut w[5..], addr);
                self.push_frame(&w[..29])
            }
            Model::M68020 | Model::M68030 => {
                // Format $B, long bus cycle fault (MC68020UM 6.4.4): SSW at
                // +10 (FB and RB for a prefetch, DF for data; RW, SIZE, FC),
                // the data cycle fault address at +16, the stage B address
                // at +36.
                w[3] = 0xB000 | fv;
                w[5] = (if prog { 0x5000 } else { 0x0100 })
                    | (if read { 0x0040 } else { 0 })
                    | size_code(size) << 4
                    | fc;
                put32(&mut w[8..], addr);
                put32(&mut w[18..], if prog { addr } else { self.r.pc });
                self.push_frame(&w[..46])
            }
            _ => {
                if vector == VEC_ADDRESS_ERROR {
                    // Format $2 with the faulted address, bit 0 clear (as
                    // FS-UAE, the bench's oracle).
                    w[3] = 0x2000 | fv;
                    put32(&mut w[4..], addr & !1);
                    return self.push_frame(&w[..6]);
                }
                // Format $7, access error (MC68040UM 8.4.6): effective
                // address at +8, SSW at +12 (RW, SIZE, TT 0, TM), fault
                // address at +20.
                w[3] = 0x7000 | fv;
                put32(&mut w[4..], addr);
                w[6] = (if read { 0x0100 } else { 0 }) | size_code(size) << 5 | fc;
                put32(&mut w[10..], addr);
                self.push_frame(&w[..30])
            }
        }
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

    /// Autovectored interrupt: the new mask is the level taken. On the
    /// 68020 to 68040 with M set, the frame goes on the master stack, then M
    /// is cleared and a format $1 throwaway frame goes on the interrupt
    /// stack (MC68020UM 6.1.7).
    fn interrupt(&mut self, level: u32) -> Res {
        let pc = self.r.pc;
        let vector = VEC_AUTOVECTOR + level;
        self.exception(vector, pc)?;
        if self.r.sr & SR_M != 0 && self.model >= Model::M68020 && self.model <= Model::M68040 {
            let sr = self.r.sr;
            self.set_sr(sr & !SR_M);
            self.push16(0x1000 | ((vector * 4) & 0x0FFF))?;
            self.push32(pc)?;
            self.push16(u32::from(sr))?;
        }
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
