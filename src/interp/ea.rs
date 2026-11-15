// SPDX-License-Identifier: MPL-2.0
//! Effective addresses of the 68000 (M68000PRM chapter 2) and of the 68020
//! (scaled index, full extension word, memory indirect: MC68020UM 2.2).
//!
//! What the 68000 does when an access faults is not in the manuals; the
//! rules here come from the SingleStepTests/m68000 data:
//!
//! - (An)+ on a read: the register moves before the access for bytes and
//!   words, after it for longs. -(An) on a read: before the access.
//!   On a write that is not read first (MOVE): (An)+ after the access;
//!   -(An) before it for bytes and words, after it for longs. A long write
//!   to -(An) writes the low word first, at the higher address.
//! - The PC stacked when a read faults is the PC as it was before the
//!   operand's extension words, except for (xxx).W and (xxx).L (after
//!   them) and for -(An) on bytes and words (2 more). Writes that come after
//!   the next prefetch (MOVE, MOVEM) set their own value in `Ea::ae_pc`.

use super::{SZ_B, SZ_L, SZ_W, sext, sz_mask};
use crate::cpu::{ACC_DATA, ACC_PROG, Cpu, Fault, Model, Res, VEC_ILLEGAL};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EaKind {
    /// `addr` is the register number
    DReg,
    /// `addr` is the register number
    AReg,
    /// `addr` is the address (32 bits, not masked)
    Mem,
    /// `addr` is the value
    Imm,
}

#[derive(Clone, Copy, Debug)]
pub struct Ea {
    pub addr: u32,
    /// Stacked PC if an access here faults
    pub ae_pc: u32,
    pub kind: EaKind,
    /// As in the opcode
    pub mode: u32,
    pub reg: u32,
    pub size: u32,
    /// (An)+ / -(An): register change still to apply
    pub step: i32,
    /// ACC_DATA, or ACC_PROG for PC relative
    pub acc: u32,
}

impl Ea {
    /// Mode 7 with register `reg`: (xxx).W 0, (xxx).L 1, (d16,PC) 2,
    /// (d8,PC,Xn) 3, #imm 4.
    #[inline(always)]
    pub fn is_mode7(&self, reg: u32) -> bool {
        self.mode == 7 && self.reg == reg
    }
}

impl Cpu {
    /// The index register of an extension word: Dn or An, its low word sign
    /// extended or all of it, times the scale (68020 on).
    #[inline(always)]
    fn index_value(&self, ext: u32, scaled: bool) -> u32 {
        let xn = ((ext >> 12) & 7) as usize;
        let x = if ext & 0x8000 != 0 {
            self.r.a[xn]
        } else {
            self.r.d[xn]
        };
        let x = if ext & 0x0800 == 0 { sext(x, SZ_W) } else { x };
        if scaled { x << ((ext >> 9) & 3) } else { x }
    }

    /// An encoding the CPU does not accept, found while decoding an operand:
    /// the illegal instruction exception, and the instruction ends here.
    #[cold]
    pub fn ea_illegal<T>(&mut self) -> Res<T> {
        self.exception_group1(VEC_ILLEGAL)?;
        Err(Fault)
    }

    /// The 68020's full extension word (MC68020UM 2.2): base and index
    /// suppress (bits 7 and 6), base displacement size (bits 5-4: 01 none,
    /// 10 word, 11 long) and the memory indirection (bits 2-0, I/IS):
    ///
    /// ```text
    ///   index used:       000 none, 001-011 pre-indexed, 101-111 post-indexed
    ///   index suppressed: 000 none, 001-011 memory indirect
    /// ```
    ///
    /// with an outer displacement of none, a word or a long (low 2 bits 01,
    /// 10, 11). The displacements follow the extension word, base first.
    /// The indirect address is a long read from data space. The reserved
    /// encodings (displacement size 00, I/IS 100, or 1xx with the index
    /// suppressed) take the illegal instruction exception.
    #[inline(never)]
    fn full_format(&mut self, base: u32, ext: u32) -> Res<u32> {
        let iis = ext & 7;
        let is = ext & 0x0040 != 0;
        if ext & 0x0030 == 0 || iis == 4 || (is && iis > 4) {
            return self.ea_illegal();
        }
        let bd = match (ext >> 4) & 3 {
            2 => sext(self.fetch16()?, SZ_W),
            3 => self.fetch32()?,
            _ => 0,
        };
        let od = match iis & 3 {
            2 => sext(self.fetch16()?, SZ_W),
            3 => self.fetch32()?,
            _ => 0,
        };
        let base = if ext & 0x0080 != 0 { 0 } else { base };
        let x = if is { 0 } else { self.index_value(ext, true) };
        if iis == 0 {
            return Ok(base.wrapping_add(bd).wrapping_add(x));
        }
        if iis & 4 != 0 {
            // post-indexed
            let m = self.read32(base.wrapping_add(bd), ACC_DATA)?;
            return Ok(m.wrapping_add(x).wrapping_add(od));
        }
        let m = self.read32(base.wrapping_add(bd).wrapping_add(x), ACC_DATA)?;
        Ok(m.wrapping_add(od))
    }

    /// (d8,An,Xn) and (d8,PC,Xn): the brief extension word. The 68000 and
    /// 68010 ignore bits 10-8; from the 68020 on, bits 10-9 scale the index
    /// and bit 8 selects the full format. The addition takes 2 cycles more.
    fn indexed(&mut self, base: u32) -> Res<u32> {
        let ext = self.fetch16()?;
        self.cycles(2);
        if self.model < Model::M68020 {
            return Ok(base
                .wrapping_add(sext(ext, SZ_B))
                .wrapping_add(self.index_value(ext, false)));
        }
        if ext & 0x0100 != 0 {
            return self.full_format(base, ext);
        }
        Ok(base
            .wrapping_add(sext(ext, SZ_B))
            .wrapping_add(self.index_value(ext, true)))
    }

    /// Works out the effective address of mode/reg for an operand of `size`
    /// bytes, fetching its extension words. (An)+ and -(An) leave the
    /// register as it is: the change is applied by `ea_read()`/`ea_write()`
    /// (or `ea_commit()`), at the point where the 68000 does it. The caller
    /// must only pass modes valid for the instruction (the table takes care
    /// of it).
    #[inline(always)]
    pub fn ea_decode(&mut self, mode: u32, reg: u32, size: u32) -> Res<Ea> {
        let pc0 = self.r.pc;
        let mut e = Ea {
            addr: 0,
            ae_pc: pc0,
            kind: EaKind::Mem,
            mode,
            reg,
            size,
            step: 0,
            acc: ACC_DATA,
        };
        let r = (reg & 7) as usize;
        // (A7)+ and -(A7) keep the stack word aligned for bytes.
        let step = if size == SZ_B && reg == 7 {
            2
        } else {
            size as i32
        };

        match mode {
            0 => {
                e.kind = EaKind::DReg;
                e.addr = reg;
            }
            1 => {
                e.kind = EaKind::AReg;
                e.addr = reg;
            }
            2 => e.addr = self.r.a[r],
            3 => {
                e.addr = self.r.a[r];
                e.step = step;
            }
            4 => {
                e.addr = self.r.a[r].wrapping_sub(step as u32);
                e.step = -step;
                if size != SZ_L {
                    e.ae_pc = pc0.wrapping_add(2);
                }
            }
            5 => e.addr = self.r.a[r].wrapping_add(sext(self.fetch16()?, SZ_W)),
            6 => e.addr = self.indexed(self.r.a[r])?,
            _ => match reg {
                0 => {
                    // (xxx).W
                    e.addr = sext(self.fetch16()?, SZ_W);
                    e.ae_pc = self.r.pc;
                }
                1 => {
                    // (xxx).L
                    e.addr = self.fetch32()?;
                    e.ae_pc = self.r.pc;
                }
                2 => {
                    // (d16,PC): relative to the extension word
                    e.addr = pc0.wrapping_add(sext(self.fetch16()?, SZ_W));
                    e.acc = ACC_PROG;
                }
                3 => {
                    // (d8,PC,Xn)
                    e.addr = self.indexed(pc0)?;
                    e.acc = ACC_PROG;
                }
                _ => {
                    // #imm (the table never sends modes 7/5-7 here)
                    e.kind = EaKind::Imm;
                    e.addr = if size == SZ_L {
                        self.fetch32()?
                    } else {
                        self.fetch16()? & sz_mask(size)
                    };
                }
            },
        }
        Ok(e)
    }

    /// The mode/reg pair of the low 6 bits of an opcode.
    #[inline(always)]
    pub fn ea_decode_op(&mut self, op: u16, size: u32) -> Res<Ea> {
        self.ea_decode(u32::from((op >> 3) & 7), u32::from(op & 7), size)
    }

    /// Applies a pending (An)+/-(An) change.
    #[inline(always)]
    pub fn ea_commit(&mut self, e: &mut Ea) {
        if e.step != 0 {
            let r = (e.reg & 7) as usize;
            self.r.a[r] = self.r.a[r].wrapping_add(e.step as u32);
            e.step = 0;
        }
    }

    /// Reads the operand (zero-extended). Address registers give their low
    /// `size` bytes.
    #[inline(always)]
    pub fn ea_read(&mut self, e: &mut Ea) -> Res<u32> {
        match e.kind {
            EaKind::DReg => return Ok(self.r.d[(e.addr & 7) as usize] & sz_mask(e.size)),
            EaKind::AReg => return Ok(self.r.a[(e.addr & 7) as usize] & sz_mask(e.size)),
            EaKind::Imm => return Ok(e.addr),
            EaKind::Mem => {}
        }
        if e.step < 0 {
            self.cycles(2); // -(An) on a read
            self.ea_commit(e);
        } else if e.step > 0 && e.size != SZ_L {
            self.ea_commit(e);
        }
        self.ae_pc = e.ae_pc;
        let v = self.read(e.addr, e.size, e.acc)?;
        self.ea_commit(e);
        Ok(v)
    }

    /// Writes the operand. A data register keeps its upper bytes; an address
    /// register gets the value sign-extended to 32 bits. Used after
    /// `ea_read()` on the same operand (read-modify-write) it does not move
    /// (An)+/-(An) again.
    #[inline(always)]
    pub fn ea_write(&mut self, e: &mut Ea, v: u32) -> Res {
        match e.kind {
            EaKind::DReg => {
                self.set_dreg(e.addr as usize, v, e.size);
                return Ok(());
            }
            EaKind::AReg => {
                self.r.a[(e.addr & 7) as usize] = sext(v, e.size);
                return Ok(());
            }
            EaKind::Imm => return Ok(()),
            EaKind::Mem => {}
        }
        if e.step < 0 && e.size != SZ_L {
            self.ea_commit(e);
        }
        self.ae_pc = e.ae_pc;
        if e.size == SZ_L && e.mode == 4 {
            // -(An): low word first
            self.write16(e.addr.wrapping_add(2), v & 0xFFFF)?;
            self.write16(e.addr, v >> 16)?;
        } else {
            self.write(e.addr, v, e.size)?;
        }
        self.ea_commit(e);
        Ok(())
    }
}
