// SPDX-License-Identifier: MPL-2.0
//! The caches of the 68020 to 68060, as far as software can see them: the
//! CACR register and the instructions that flush or invalidate. The engines
//! keep no copy of the 68k's caches; what matters is that code which the
//! 68k invalidates is also dropped by the JIT (`relevo_invalidate()`).
//!
//! The CACR layouts are those of MC68020UM 6.3, MC68030UM 6.4, MC68040UM
//! 4.1 and MC68060UM 5.3.

use crate::cpu::{Cpu, Model};

/// 68020: E (0) and F (1); C (3) and CE (2) are write-only.
const CACR_020: u32 = 0x0000_0003;
/// 68030: WA (13), DBE (12), FD (9), ED (8), IBE (4), FI (1), EI (0); CD
/// (11), CED (10), C (3) and CE (2) are write-only.
const CACR_030: u32 = 0x0000_3313;
/// 68040: DE (31), IE (15).
const CACR_040: u32 = 0x8000_8000;
/// 68060: EDC (31), NAD (30), ESB (29), DPI (28), FOC (27), EBC (23), EIC
/// (15), NAI (14), FIC (13); CABC (22) and CUBC (21) are write-only.
const CACR_060: u32 = 0xF880_E000;

/// 68020/030: clear the instruction cache
const CACR_C: u32 = 0x0000_0008;
/// 68020/030: clear one entry
const CACR_CE: u32 = 0x0000_0004;

/// CACR bits that hold a value on each model; the rest read as 0.
fn cacr_mask(model: Model) -> u32 {
    match model {
        Model::M68020 => CACR_020,
        Model::M68030 => CACR_030,
        Model::M68040 => CACR_040,
        Model::M68060 => CACR_060,
        _ => 0,
    }
}

impl Cpu {
    /// A write to CACR (MOVEC): keeps the bits the model has and acts on the
    /// write-only ones of the 68020 and 68030 (C clears the instruction
    /// cache, CE the entry that CAAR points at; CD and CED clear the data
    /// cache, which needs nothing).
    pub fn write_cacr(&mut self, v: u32) {
        if self.model == Model::M68020 || self.model == Model::M68030 {
            if v & CACR_C != 0 {
                self.invalidate_all();
            } else if v & CACR_CE != 0 {
                self.invalidate_line(self.r.caar);
            }
        }
        self.r.cacr = v & cacr_mask(self.model);
    }

    /// Code at `[address, address + size)` changed or must be fetched
    /// again. The interpreter keeps no translated code; the JIT (RR3) will.
    pub fn invalidate(&mut self, _address: u32, _size: u32) {}

    /// Instruction cache invalidation: a 16-byte line.
    pub fn invalidate_line(&mut self, address: u32) {
        self.invalidate(address & !15, 16);
    }

    /// A page (8 KB, the larger of the 68040's page sizes).
    pub fn invalidate_page(&mut self, address: u32) {
        self.invalidate(address & !0x1FFF, 0x2000);
    }

    /// Everything. The size is 32 bits: the whole space takes two halves.
    pub fn invalidate_all(&mut self) {
        self.invalidate(0, 0x8000_0000);
        self.invalidate(0x8000_0000, 0x8000_0000);
    }
}
