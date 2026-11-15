// SPDX-License-Identifier: MPL-2.0
//! Memory accesses: the host's (`relevo_read()`, `relevo_write()`), which
//! never fault, and the CPU's own, which may raise a group 0 exception.
//!
//! The fast path is a direct access to a RAM (or ROM, to read) page that
//! does not cross the page's end. Everything else takes the slow path: host
//! pages, unmapped pages, ROM writes and accesses across a page boundary.

use crate::cpu::{ACC_DATA, ACC_READ, ACC_WRITE, Cpu, Res};
use crate::window::{PAGE_HOST, PAGE_NONE, PAGE_RAM, PAGE_ROM, in_page};

impl Cpu {
    // ---- Accesses that never fault (masked here) ----------------------

    #[inline]
    pub fn mem_read8(&mut self, addr: u32) -> u32 {
        let a = addr & self.addr_mask;
        match self.mem.read8(a) {
            Some(v) => v,
            None => self.mem_read_slow(a, 1),
        }
    }

    #[inline]
    pub fn mem_read16(&mut self, addr: u32) -> u32 {
        let a = addr & self.addr_mask;
        match self.mem.read16(a) {
            Some(v) => v,
            None => self.mem_read_slow(a, 2),
        }
    }

    #[inline]
    pub fn mem_read32(&mut self, addr: u32) -> u32 {
        let a = addr & self.addr_mask;
        match self.mem.read32(a) {
            Some(v) => v,
            None => self.mem_read_slow(a, 4),
        }
    }

    #[inline]
    pub fn mem_write8(&mut self, addr: u32, v: u32) {
        let a = addr & self.addr_mask;
        if !self.mem.write8(a, v) {
            self.mem_write_slow(a, v, 1);
        }
    }

    #[inline]
    pub fn mem_write16(&mut self, addr: u32, v: u32) {
        let a = addr & self.addr_mask;
        if !self.mem.write16(a, v) {
            self.mem_write_slow(a, v, 2);
        }
    }

    #[inline]
    pub fn mem_write32(&mut self, addr: u32, v: u32) {
        let a = addr & self.addr_mask;
        if !self.mem.write32(a, v) {
            self.mem_write_slow(a, v, 4);
        }
    }

    /// `size` bytes at the masked address `a`, inside one page.
    fn read_in_page(&mut self, a: u32, size: u32) -> u32 {
        match self.mem.kind(a) {
            PAGE_RAM | PAGE_ROM => {
                let v = match size {
                    1 => self.mem.read8(a),
                    2 => self.mem.read16(a),
                    _ => self.mem.read32(a),
                };
                v.unwrap_or(0)
            }
            PAGE_HOST => {
                let v = self.host_read(a, size);
                if size == 4 {
                    v
                } else {
                    v & ((1 << (8 * size)) - 1)
                }
            }
            // Unmapped: the CPU's own accesses never get here (bus error).
            _ => 0,
        }
    }

    fn write_in_page(&mut self, a: u32, v: u32, size: u32) {
        match self.mem.kind(a) {
            PAGE_RAM => {
                match size {
                    1 => self.mem.write8(a, v),
                    2 => self.mem.write16(a, v),
                    _ => self.mem.write32(a, v),
                };
            }
            PAGE_HOST => self.host_write(a, v, size),
            // ROM: dropped; unmapped: as reads
            _ => {}
        }
    }

    /// `a` is already masked.
    #[inline(never)]
    pub fn mem_read_slow(&mut self, a: u32, size: u32) -> u32 {
        if in_page(a, size) {
            return self.read_in_page(a, size);
        }
        // Crosses a page: byte by byte, each address masked again (it may
        // wrap around the end of the address space).
        let mut v = 0;
        for i in 0..size {
            v = v << 8 | self.read_in_page(a.wrapping_add(i) & self.addr_mask, 1);
        }
        v
    }

    #[inline(never)]
    pub fn mem_write_slow(&mut self, a: u32, v: u32, size: u32) {
        if in_page(a, size) {
            self.write_in_page(a, v, size);
            return;
        }
        for i in 0..size {
            let byte = (v >> (8 * (size - 1 - i))) & 0xFF;
            self.write_in_page(a.wrapping_add(i) & self.addr_mask, byte, 1);
        }
    }

    // ---- The CPU's own accesses ---------------------------------------
    //
    // These may raise a group 0 exception: an address error for a word or
    // long access at an odd address (68000 and 68010), a bus error for an
    // unmapped page. They are only used while the CPU runs. `acc` is
    // ACC_DATA or ACC_PROG. Addresses are given unmasked: an address error
    // stacks the whole 32 bits.

    #[inline(always)]
    pub fn read8(&mut self, addr: u32, acc: u32) -> Res<u32> {
        self.cycles += 4;
        match self.mem.read8(addr & self.addr_mask) {
            Some(v) => Ok(v),
            None => self.read_slow(addr, 1, acc),
        }
    }

    #[inline(always)]
    pub fn read16(&mut self, addr: u32, acc: u32) -> Res<u32> {
        if addr & 1 != 0 && self.align_check {
            return Err(self.address_error(addr, acc | ACC_READ));
        }
        self.cycles += 4;
        match self.mem.read16(addr & self.addr_mask) {
            Some(v) => Ok(v),
            None => self.read_slow(addr, 2, acc),
        }
    }

    #[inline(always)]
    pub fn read32(&mut self, addr: u32, acc: u32) -> Res<u32> {
        if addr & 1 != 0 && self.align_check {
            return Err(self.address_error(addr, acc | ACC_READ));
        }
        self.cycles += 8;
        match self.mem.read32(addr & self.addr_mask) {
            Some(v) => Ok(v),
            None => self.read_slow(addr, 4, acc),
        }
    }

    #[inline(always)]
    pub fn write8(&mut self, addr: u32, v: u32) -> Res {
        self.cycles += 4;
        if self.mem.write8(addr & self.addr_mask, v) {
            Ok(())
        } else {
            self.write_slow(addr, v, 1, ACC_DATA)
        }
    }

    #[inline(always)]
    pub fn write16(&mut self, addr: u32, v: u32) -> Res {
        if addr & 1 != 0 && self.align_check {
            return Err(self.address_error(addr, ACC_DATA | ACC_WRITE));
        }
        self.cycles += 4;
        if self.mem.write16(addr & self.addr_mask, v) {
            Ok(())
        } else {
            self.write_slow(addr, v, 2, ACC_DATA)
        }
    }

    #[inline(always)]
    pub fn write32(&mut self, addr: u32, v: u32) -> Res {
        if addr & 1 != 0 && self.align_check {
            return Err(self.address_error(addr, ACC_DATA | ACC_WRITE));
        }
        self.cycles += 8;
        if self.mem.write32(addr & self.addr_mask, v) {
            Ok(())
        } else {
            self.write_slow(addr, v, 4, ACC_DATA)
        }
    }

    /// An unmapped page is a bus error. `addr` is unmasked; the alignment
    /// check is already done.
    fn check_mapped(&mut self, addr: u32, size: u32, acc: u32) -> Res {
        for i in 0..size {
            let a = addr.wrapping_add(i) & self.addr_mask;
            if self.mem.kind(a) == PAGE_NONE {
                self.fault_size = size;
                return Err(self.bus_error(addr, acc));
            }
        }
        Ok(())
    }

    #[inline(never)]
    fn read_slow(&mut self, addr: u32, size: u32, acc: u32) -> Res<u32> {
        self.check_mapped(addr, size, acc | ACC_READ)?;
        Ok(self.mem_read_slow(addr & self.addr_mask, size))
    }

    #[inline(never)]
    fn write_slow(&mut self, addr: u32, v: u32, size: u32, acc: u32) -> Res {
        self.check_mapped(addr, size, acc | ACC_WRITE)?;
        self.mem_write_slow(addr & self.addr_mask, v, size);
        Ok(())
    }
}
