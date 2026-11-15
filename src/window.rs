// SPDX-License-Identifier: MPL-2.0
//! The 68k memory window: the block of memory the host gives, and the page
//! table that says what each 64 KB page of the 4 GB address space is.
//!
//! This module holds the raw pointer to the host's memory and is one of the
//! two places with `unsafe`. Its invariant: a page is RAM or ROM only if it
//! lies wholly inside the block, so a direct access to such a page that does
//! not cross the page's end is always in bounds. [`Window::map`] keeps it;
//! the page table is private to this module.
#![allow(unsafe_code)]

use alloc::boxed::Box;
use alloc::vec;

pub const PAGE_SHIFT: u32 = 16;
pub const PAGE_SIZE: u32 = 1 << PAGE_SHIFT;
pub const PAGE_COUNT: usize = 1 << (32 - PAGE_SHIFT);

/// Page kinds, as in `relevo_page_kind`.
pub const PAGE_NONE: u8 = 0;
pub const PAGE_RAM: u8 = 1;
pub const PAGE_ROM: u8 = 2;
pub const PAGE_HOST: u8 = 3;
/// An internal kind: RAM holding code the JIT has translated. Reads are
/// direct; writes take the slow path, which drops the translations they
/// touch. The JIT's flush turns these pages back into RAM.
pub const PAGE_RAM_CODE: u8 = 4;

/// Whether `size` bytes at the masked address `a` stay inside one page.
#[inline(always)]
pub fn in_page(a: u32, size: u32) -> bool {
    (a & (PAGE_SIZE - 1)) <= PAGE_SIZE - size
}

/// RAM, ROM or RAM with translated code: read directly.
#[inline(always)]
pub fn direct_read(kind: u8) -> bool {
    (0b10110 >> kind) & 1 != 0
}

pub struct Window {
    base: *mut u8,
    size: u64,
    pages: Box<[u8; PAGE_COUNT]>,
}

impl Window {
    /// A window on `size` bytes at `base`, every page unmapped.
    ///
    /// # Safety
    ///
    /// Unless `size` is 0, `base` must be valid for reads and writes of
    /// `size` bytes for as long as the window lives (the host's promise in
    /// `relevo_config`). Others (the host) may access the memory too, but not
    /// while a method of the window runs.
    pub unsafe fn new(base: *mut u8, size: u64) -> Window {
        let pages = vec![PAGE_NONE; PAGE_COUNT].into_boxed_slice();
        Window {
            base,
            size,
            pages: pages.try_into().expect("PAGE_COUNT entries"),
        }
    }

    /// Maps `[address, address + size)` as `kind`. Both must be multiples of
    /// the page size; RAM and ROM must lie inside the memory block. Returns
    /// false (and changes nothing) otherwise.
    pub fn map(&mut self, address: u32, size: u64, kind: u8) -> bool {
        let end = u64::from(address) + size;
        if address % PAGE_SIZE != 0
            || size % u64::from(PAGE_SIZE) != 0
            || end > 1u64 << 32
            || kind > PAGE_HOST
        {
            return false;
        }
        if (kind == PAGE_RAM || kind == PAGE_ROM) && (self.base.is_null() || end > self.size) {
            return false;
        }
        let first = (address >> PAGE_SHIFT) as usize;
        let last = (end >> PAGE_SHIFT) as usize;
        self.pages[first..last].fill(kind);
        true
    }

    /// Marks the RAM page `page` as holding translated code, or (`code`
    /// false) a page with code back as RAM. Other pages do not change.
    pub fn set_code(&mut self, page: usize, code: bool) {
        let k = &mut self.pages[page];
        if code && *k == PAGE_RAM {
            *k = PAGE_RAM_CODE;
        } else if !code && *k == PAGE_RAM_CODE {
            *k = PAGE_RAM;
        }
    }

    /// The kind of page `page` (0 to PAGE_COUNT - 1).
    #[inline(always)]
    pub fn page_kind(&self, page: usize) -> u8 {
        self.pages[page]
    }

    /// The size of the memory block.
    pub fn size(&self) -> u64 {
        self.size
    }

    #[cfg_attr(not(target_arch = "x86_64"), allow(dead_code))]
    /// The start of the memory block (null if none): translated code
    /// addresses the 68k memory from it.
    pub fn base(&self) -> *mut u8 {
        self.base
    }

    /// The kind of the page of the masked address `a`.
    #[inline(always)]
    pub fn kind(&self, a: u32) -> u8 {
        self.pages[(a >> PAGE_SHIFT) as usize]
    }

    #[inline(always)]
    fn ptr(&self, a: u32, size: u32) -> *mut u8 {
        debug_assert!(u64::from(a) + u64::from(size) <= self.size);
        // SAFETY: callers only get here for a RAM or ROM page and an access
        // that does not leave it, which the invariant puts inside the block.
        unsafe { self.base.add(a as usize) }
    }

    /// A byte from a RAM or ROM page; None for other pages.
    #[inline(always)]
    pub fn read8(&self, a: u32) -> Option<u32> {
        if direct_read(self.kind(a)) {
            // SAFETY: see ptr().
            Some(u32::from(unsafe { self.ptr(a, 1).read() }))
        } else {
            None
        }
    }

    /// A big-endian word from a RAM or ROM page, if it does not cross the
    /// page's end; None otherwise.
    #[inline(always)]
    pub fn read16(&self, a: u32) -> Option<u32> {
        if direct_read(self.kind(a)) && in_page(a, 2) {
            // SAFETY: see ptr(); [u8; 2] has no alignment needs.
            let b = unsafe { self.ptr(a, 2).cast::<[u8; 2]>().read() };
            Some(u32::from(u16::from_be_bytes(b)))
        } else {
            None
        }
    }

    /// A big-endian long, as read16().
    #[inline(always)]
    pub fn read32(&self, a: u32) -> Option<u32> {
        if direct_read(self.kind(a)) && in_page(a, 4) {
            // SAFETY: see ptr().
            let b = unsafe { self.ptr(a, 4).cast::<[u8; 4]>().read() };
            Some(u32::from_be_bytes(b))
        } else {
            None
        }
    }

    /// Writes `size` bytes big-endian to a RAM page or one with code, if
    /// they do not cross the page's end; false (nothing written)
    /// otherwise.
    pub fn write_ram(&mut self, a: u32, v: u32, size: u32) -> bool {
        let kind = self.kind(a);
        if (kind != PAGE_RAM && kind != PAGE_RAM_CODE) || !in_page(a, size) {
            return false;
        }
        // SAFETY: see ptr().
        unsafe {
            match size {
                1 => self.ptr(a, 1).write(v as u8),
                2 => self
                    .ptr(a, 2)
                    .cast::<[u8; 2]>()
                    .write((v as u16).to_be_bytes()),
                _ => self.ptr(a, 4).cast::<[u8; 4]>().write(v.to_be_bytes()),
            }
        }
        true
    }

    /// Writes a byte to a RAM page; false (nothing written) for other pages.
    #[inline(always)]
    pub fn write8(&mut self, a: u32, v: u32) -> bool {
        if self.kind(a) == PAGE_RAM {
            // SAFETY: see ptr().
            unsafe { self.ptr(a, 1).write(v as u8) };
            true
        } else {
            false
        }
    }

    /// Writes a big-endian word to a RAM page, if it does not cross the
    /// page's end; false (nothing written) otherwise.
    #[inline(always)]
    pub fn write16(&mut self, a: u32, v: u32) -> bool {
        if self.kind(a) == PAGE_RAM && in_page(a, 2) {
            // SAFETY: see ptr().
            unsafe {
                self.ptr(a, 2)
                    .cast::<[u8; 2]>()
                    .write((v as u16).to_be_bytes())
            };
            true
        } else {
            false
        }
    }

    /// A big-endian long, as write16().
    #[inline(always)]
    pub fn write32(&mut self, a: u32, v: u32) -> bool {
        if self.kind(a) == PAGE_RAM && in_page(a, 4) {
            // SAFETY: see ptr().
            unsafe { self.ptr(a, 4).cast::<[u8; 4]>().write(v.to_be_bytes()) };
            true
        } else {
            false
        }
    }
}
