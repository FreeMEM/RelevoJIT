// SPDX-License-Identifier: MPL-2.0
//! Temporary stub.
use super::OpTable;
use crate::cpu::Cpu;
use crate::fpx;
impl Cpu {
    pub fn fpu_reset(&mut self) {
        self.r.fp = [fpx::default_nan(); 8];
        self.r.fpcr = 0;
        self.r.fpsr = 0;
        self.r.fpiar = 0;
        self.fpu_null = true;
        self.fpu_pending = 0;
    }
}
pub fn fill(_tb: &mut OpTable) {}
