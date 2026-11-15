// SPDX-License-Identifier: MPL-2.0
//! The JIT engine: its dispatcher and its cache of translations.
//!
//! The dispatcher runs translated blocks and falls back to the interpreter,
//! one instruction at a time, for what it does not translate. Translated
//! blocks never fault: before the first side effect of an instruction they
//! check every memory access, and if one is not a plain access to RAM (or
//! ROM, for reads) they leave the block with the state from before that
//! instruction ("bail"), which the interpreter then runs. So exceptions and
//! the 68000's fault frames all come from the interpreter, exactly as
//! without the JIT. An access to a host page (or a ROM write) does not
//! leave: the block calls the interpreter for that instruction and then
//! goes on to the next block (`jit_host_step()`).
//!
//! Blocks are found by the 68k address of their first instruction. A block
//! runs from its first instruction up to a jump or branch, an instruction
//! the JIT does not translate, `MAX_INSNS` instructions, the budget, or a
//! 68k register more than the x86 registers it can hold.
//!
//! Chaining: a block that leaves for a PC known when it was translated (a
//! branch, the next instruction) does it through a jump that first goes
//! back to the dispatcher; the dispatcher then patches it to go straight to
//! the block at that PC, and records the link in that block so that
//! dropping it puts the jump back. Indirect jumps (RTS, JMP (An)) look the
//! PC up in a small cache of targets from translated code. Every block
//! checks the budget and the attention flag when it is entered, so a chain
//! of blocks stops where the dispatcher would.
//!
//! Between blocks the dispatcher does what the interpreter does between
//! instructions: interrupts, trace, STOP, the stop request, the budget and
//! the exit at PC 0.
//!
//! Changed code: translated code is not checked when it runs; writes into
//! it are caught instead. The pages with translated code are marked
//! (`PAGE_RAM_CODE` in the page table, `JMAP_CODE` in the JIT's finer map);
//! a write there by a block leaves for the interpreter if it touches
//! translated code, and a write by the interpreter or the host
//! (`relevo_write()`) drops the translations it touches
//! (`jit_code_written()`). The blocks are listed by the 4 KB page where
//! they start, with a mask of the 64-byte chunks of each page that hold
//! translated code, so that a write finds the blocks it touches quickly.
//! Whoever writes the memory block directly calls `relevo_invalidate()`.
//!
//! `unsafe` lives in `codemem` (the executable memory) and `native` (the
//! call into translated code and its calls back); this module and the code
//! generator deny it.

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::sync::atomic::Ordering::Relaxed;

use crate::cpu::{Cpu, Fpu, Model, OPT_EXIT_ON_PC_ZERO, Res, SR_T1, SR_X, Status};
use crate::window::{
    PAGE_COUNT, PAGE_NONE, PAGE_RAM, PAGE_RAM_CODE, PAGE_ROM, PAGE_SHIFT, PAGE_SIZE, Window,
    direct_read,
};

mod codemem;
mod decode;
mod native;
mod x86_64;

use codemem::CodeMem;
use decode::{Ctx, F_NZVC, F_X, Insn};
use x86_64::emit::XBuf;
use x86_64::translate::{self, Env, HOST_REGS, Tr};

/// Instructions per block
pub const MAX_INSNS: usize = 64;
/// Buckets, by 68k address
const HASH_SIZE: usize = 4096;
/// Block descriptors
const POOL_SIZE: usize = 65536;
/// Chained jumps
const LINK_SIZE: usize = 2 * POOL_SIZE;
/// The cache of indirect jump targets
pub const IND_SIZE: usize = 4096;
/// Executable memory per CPU
const CODE_SIZE: usize = 32 << 20;

/// The JIT's map of the 68k address space, in pages of 4 KB: what a
/// translated access may do directly.
pub const MAP_SHIFT: u32 = 12;
const MAP_SIZE: usize = 1 << (32 - MAP_SHIFT);
/// not RAM or ROM
pub const JMAP_NO_READ: u8 = 0x01;
/// not RAM
pub const JMAP_NO_WRITE: u8 = 0x02;
/// RAM with translated code
pub const JMAP_CODE: u8 = 0x04;
/// Translated code is tracked in chunks of 64 bytes of each 4 KB page.
const CHUNK_SHIFT: u32 = 6;
/// 4 KB pages per 64 KB page
const MAP_PER_PAGE: usize = 1 << (PAGE_SHIFT - MAP_SHIFT);

/// What a block returns to the dispatcher (the low 32 bits of enter()'s
/// result; the PC is set in every case).
/// Go on at the PC.
pub const EXIT_NORMAL: u32 = 0;
/// The interpreter runs the PC.
pub const EXIT_BAIL: u32 = 1;
/// As NORMAL; the high 32 bits are the offset of a jump to patch to the
/// block at the PC.
pub const EXIT_CHAIN: u32 = 2;

/// No block (an index into the pool).
const NONE: u32 = u32::MAX;

/// A translation: the code for the 68k instructions in [start, start+len)
/// (masked addresses), entered when the PC is `pc`.
#[derive(Clone, Copy)]
struct Block {
    /// in the bucket, or free
    next: u32,
    /// in the list of its first 4 KB page
    page_next: u32,
    pc: u32,
    start: u32,
    len: u32,
    /// offset of its code; None: run the interpreter here
    code: Option<usize>,
    /// jumps chained to it (links)
    links: u32,
    ninsns: u16,
    /// shortened by the budget
    cut: bool,
    /// only ROM pages
    rom: bool,
    /// in a page list
    listed: bool,
}

const EMPTY_BLOCK: Block = Block {
    next: NONE,
    page_next: NONE,
    pc: 0,
    start: 0,
    len: 0,
    code: None,
    links: 0,
    ninsns: 0,
    cut: false,
    rom: false,
    listed: false,
};

/// A jump of some block patched to go straight to another block.
#[derive(Clone, Copy, Default)]
struct Link {
    /// the target's next link; 0 ends
    next: u32,
    /// the jump's rel32, in the code memory
    site: u32,
}

/// An entry of the cache of indirect jump targets (RTS, JMP (An)): the
/// translated code for a PC. Translated code reads it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Ind {
    /// 1 (odd): empty
    pc: u32,
    pad: u32,
    /// the address of the code
    code: usize,
}

const EMPTY_IND: Ind = Ind {
    pc: 1,
    pad: 0,
    code: 0,
};

/// For the tests: `jit_stats` of the C version.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    /// blocks translated
    pub blocks: u64,
    /// instructions translated
    pub insns: u64,
    /// blocks entered
    pub runs: u64,
    /// left a block for the interpreter
    pub bails: u64,
    /// instructions run by the interpreter
    pub interp: u64,
    /// whole cache flushes
    pub flushes: u64,
    /// jumps chained
    pub links: u64,
    /// blocks dropped (code changed, invalidate)
    pub dropped: u64,
    /// instructions a block ran in the interpreter for a host page
    pub host: u64,
}

pub struct Jit {
    /// The cache of indirect targets and the map: translated code reads
    /// them through `Cpu::jit_ind` and `Cpu::jit_map`, so they never move.
    ind: Box<[Ind]>,
    map: Box<[u8]>,
    mem: CodeMem,
    /// bytes of mem in use
    used: usize,
    /// what flushes keep (the trampoline and stubs)
    base_used: usize,
    env: Env,
    has_movbe: bool,
    has_lahf: bool,
    /// next instruction: interpreter
    force_interp: bool,
    hash: Box<[u32]>,
    pool: Vec<Block>,
    free_list: u32,
    links: Vec<Link>,
    links_free: u32,
    /// Per 4 KB page of the memory block: the blocks that start there and
    /// the chunks with translated code.
    npages: u32,
    page_head: Vec<u32>,
    page_chunks: Vec<u64>,
    /// begin_write() nesting
    write_depth: u32,
    pub stats: Stats,
    insns: Vec<Insn>,
    /// Opcodes the interpreter's table executes on this model
    legal: Box<[bool]>,
    tr: Box<Tr>,
}

fn bucket_of(pc: u32) -> usize {
    ((pc >> 1 ^ pc >> 13) as usize) & (HASH_SIZE - 1)
}

fn ind_index(pc: u32) -> usize {
    (pc >> 1) as usize & (IND_SIZE - 1)
}

/// The chunks of page `p` that [lo, hi) covers.
fn chunks_of(lo: u64, hi: u64, p: u32) -> u64 {
    let pb = u64::from(p) << MAP_SHIFT;
    let pe = pb + (1 << MAP_SHIFT);
    let lo = lo.max(pb);
    let hi = hi.min(pe);
    if lo >= hi {
        return 0;
    }
    let first = (lo - pb) >> CHUNK_SHIFT;
    let last = (hi - 1 - pb) >> CHUNK_SHIFT;
    let m = if last == 63 {
        !0
    } else {
        (1u64 << (last + 1)) - 1
    };
    m & !((1u64 << first) - 1)
}

impl Block {
    fn first_page(&self) -> u32 {
        self.start >> MAP_SHIFT
    }

    fn last_page(&self) -> u32 {
        ((u64::from(self.start) + u64::from(self.len) - 1) >> MAP_SHIFT) as u32
    }

    fn overlaps(&self, lo: u64, hi: u64) -> bool {
        self.len != 0
            && u64::from(self.start) < hi
            && lo < u64::from(self.start) + u64::from(self.len)
    }
}

/// Whether this build and this machine can run the JIT.
pub fn available() -> bool {
    native::available()
}

impl Jit {
    /// The JIT for a CPU, or None if there is no executable memory.
    fn new(cpu: &Cpu, legal: Box<[bool]>) -> Option<Box<Jit>> {
        if !available() {
            return None;
        }
        let mut mem = CodeMem::new(CODE_SIZE)?;
        let cb = native::callbacks();
        let has_movbe = native::has_movbe();
        let has_lahf = native::has_lahf();
        let mut b = XBuf::default();
        let stubs = translate::init(&mut b, cb.access_check);
        if b.b.len() > mem.size() || !mem.begin_write() {
            return None;
        }
        mem.write(0, &b.b);
        mem.end_write();
        let env = Env {
            model: cpu.model,
            addr_mask: cpu.addr_mask,
            align_check: cpu.align_check,
            movbe: has_movbe,
            lazy: has_lahf,
            exit_off: stubs.exit_off,
            check_off: stubs.check_off,
            ccr_off: stubs.ccr_off,
            host_step: cb.host_step,
            call_handler: cb.call_handler,
            fpu_op: cb.fpu_op,
            fpu_branch: cb.fpu_branch,
        };
        let npages = (cpu.mem_size() >> MAP_SHIFT) as u32;
        let mut links = Vec::with_capacity(1024);
        links.push(Link::default()); // 0 ends a list
        Some(Box::new(Jit {
            ind: vec![EMPTY_IND; IND_SIZE].into_boxed_slice(),
            map: vec![0u8; MAP_SIZE].into_boxed_slice(),
            mem,
            used: b.b.len(),
            base_used: b.b.len(),
            env,
            has_movbe,
            has_lahf,
            force_interp: false,
            hash: vec![NONE; HASH_SIZE].into_boxed_slice(),
            pool: Vec::new(),
            free_list: NONE,
            links,
            links_free: 0,
            npages,
            page_head: vec![NONE; npages as usize],
            page_chunks: vec![0; npages as usize],
            write_depth: 0,
            stats: Stats::default(),
            insns: Vec::with_capacity(MAX_INSNS),
            legal,
            tr: Box::new(Tr::new(env)),
        }))
    }

    fn write_begin(&mut self) -> bool {
        if self.write_depth == 0 && !self.mem.begin_write() {
            return false;
        }
        self.write_depth += 1;
        true
    }

    fn write_end(&mut self) {
        self.write_depth -= 1;
        if self.write_depth == 0 {
            self.mem.end_write();
        }
    }

    /// Points the rel32 of a jump at `site` to the offset `to`.
    fn patch_jump(&mut self, site: u32, to: usize) {
        let rel = (to as i64 - (i64::from(site) + 4)) as i32;
        self.mem.write(site as usize, &rel.to_le_bytes());
    }

    fn ind_clear(&mut self) {
        self.ind.fill(EMPTY_IND);
    }

    /// Sets the map from the page table, and the pages with code back to
    /// RAM.
    fn rebuild_map(&mut self, w: &mut Window) {
        for p in 0..PAGE_COUNT {
            w.set_code(p, false);
            let kind = w.page_kind(p);
            let mut v = 0;
            if !direct_read(kind) {
                v |= JMAP_NO_READ;
            }
            if kind != PAGE_RAM {
                v |= JMAP_NO_WRITE;
            }
            self.map[p * MAP_PER_PAGE..(p + 1) * MAP_PER_PAGE].fill(v);
        }
    }

    /// Drops every translation (the page map changed, or the memory is
    /// full).
    fn flush(&mut self, w: &mut Window) {
        self.hash.fill(NONE);
        self.pool.clear();
        self.free_list = NONE;
        self.links.truncate(1);
        self.links_free = 0;
        self.used = self.base_used;
        self.page_head.fill(NONE);
        self.page_chunks.fill(0);
        self.ind_clear();
        self.rebuild_map(w);
        self.stats.flushes += 1;
    }

    // ---- Links ----

    fn link_block(&mut self, site: u32, b: u32) {
        let Some(code) = self.pool[b as usize].code else {
            return;
        };
        let k = if self.links_free != 0 {
            let k = self.links_free;
            self.links_free = self.links[k as usize].next;
            k
        } else if self.links.len() < LINK_SIZE {
            self.links.push(Link::default());
            (self.links.len() - 1) as u32
        } else {
            return;
        };
        if !self.write_begin() {
            self.links[k as usize].next = self.links_free;
            self.links_free = k;
            return;
        }
        self.patch_jump(site, code);
        self.write_end();
        let blk = &mut self.pool[b as usize];
        self.links[k as usize] = Link {
            next: blk.links,
            site,
        };
        blk.links = k;
        self.stats.links += 1;
    }

    /// Puts back every jump chained to `b` (to the exit that follows it).
    /// Returns false if the code memory cannot be written.
    fn unlink_block(&mut self, b: u32) -> bool {
        if self.pool[b as usize].links == 0 {
            return true;
        }
        if !self.write_begin() {
            return false;
        }
        let mut k = self.pool[b as usize].links;
        while k != 0 {
            let l = self.links[k as usize];
            self.patch_jump(l.site, l.site as usize + 4);
            self.links[k as usize].next = self.links_free;
            self.links_free = k;
            k = l.next;
        }
        self.write_end();
        self.pool[b as usize].links = 0;
        true
    }

    // ---- Pages with code ----

    fn mark_block(&mut self, w: &mut Window, b: u32) {
        let blk = self.pool[b as usize];
        let p0 = blk.first_page();
        if blk.len == 0 || p0 >= self.npages {
            return;
        }
        self.pool[b as usize].page_next = self.page_head[p0 as usize];
        self.page_head[p0 as usize] = b;
        self.pool[b as usize].listed = true;
        if blk.rom {
            return;
        }
        let lo = u64::from(blk.start);
        let hi = lo + u64::from(blk.len);
        let mut p = p0;
        while p <= blk.last_page() && p < self.npages {
            self.page_chunks[p as usize] |= chunks_of(lo, hi, p);
            self.map[p as usize] |= JMAP_CODE;
            w.set_code((p >> (PAGE_SHIFT - MAP_SHIFT)) as usize, true);
            p += 1;
        }
    }

    /// The chunks of page `p` with code, from the blocks listed in it and
    /// in the page before (whose blocks may run into it).
    fn recompute_page(&mut self, p: u32) {
        let mut m = 0;
        for q in p.saturating_sub(1)..=p {
            let mut b = self.page_head[q as usize];
            while b != NONE {
                let blk = &self.pool[b as usize];
                if !blk.rom {
                    m |= chunks_of(
                        u64::from(blk.start),
                        u64::from(blk.start) + u64::from(blk.len),
                        p,
                    );
                }
                b = blk.page_next;
            }
        }
        self.page_chunks[p as usize] = m;
        if m == 0 {
            self.map[p as usize] &= !JMAP_CODE;
        }
    }

    fn unlist(&mut self, b: u32) {
        let blk = self.pool[b as usize];
        if !blk.listed {
            return;
        }
        let p0 = blk.first_page() as usize;
        if self.page_head[p0] == b {
            self.page_head[p0] = blk.page_next;
        } else {
            let mut q = self.page_head[p0];
            while self.pool[q as usize].page_next != b {
                q = self.pool[q as usize].page_next;
            }
            self.pool[q as usize].page_next = blk.page_next;
        }
        self.pool[b as usize].listed = false;
        if blk.rom {
            return;
        }
        let mut q = blk.first_page();
        while q <= blk.last_page() && q < self.npages {
            self.recompute_page(q);
            q += 1;
        }
    }

    fn free_block(&mut self, b: u32) {
        self.pool[b as usize].next = self.free_list;
        self.free_list = b;
    }

    fn drop_block(&mut self, w: &mut Window, b: u32) {
        let blk = self.pool[b as usize];
        let k = bucket_of(blk.pc);
        if self.hash[k] == b {
            self.hash[k] = blk.next;
        } else {
            let mut q = self.hash[k];
            while self.pool[q as usize].next != b {
                q = self.pool[q as usize].next;
            }
            self.pool[q as usize].next = blk.next;
        }
        self.unlist(b);
        if !self.unlink_block(b) {
            // Cannot put the jumps back: start again from nothing.
            self.flush(w);
            return;
        }
        let e = &mut self.ind[ind_index(blk.pc)];
        if e.pc == blk.pc {
            *e = EMPTY_IND;
        }
        self.stats.dropped += 1;
        self.free_block(b);
    }

    /// A listed block with code in [lo, hi) (masked addresses), or NONE.
    fn find_overlap(&self, lo: u64, hi: u64) -> u32 {
        let p0 = (lo >> MAP_SHIFT) as u32;
        let p1 = ((hi - 1) >> MAP_SHIFT) as u32;
        let mut p = p0.saturating_sub(1);
        while p <= p1 && p < self.npages {
            let mut b = self.page_head[p as usize];
            while b != NONE {
                if self.pool[b as usize].overlaps(lo, hi) {
                    return b;
                }
                b = self.pool[b as usize].page_next;
            }
            p += 1;
        }
        NONE
    }

    /// Whether a write to [lo, hi) may touch translated RAM code.
    fn may_hit_code(&self, lo: u64, hi: u64) -> bool {
        let p1 = ((hi - 1) >> MAP_SHIFT) as u32;
        let mut p = (lo >> MAP_SHIFT) as u32;
        while p <= p1 {
            if p < self.npages
                && self.map[p as usize] & JMAP_CODE != 0
                && self.page_chunks[p as usize] & chunks_of(lo, hi, p) != 0
            {
                return true;
            }
            p += 1;
        }
        false
    }

    fn invalidate_range(&mut self, w: &mut Window, lo: u64, hi: u64) {
        if lo >= hi {
            return;
        }
        if (hi - lo) >> MAP_SHIFT > 4096 {
            // A large range: look at every block.
            for k in 0..HASH_SIZE {
                let mut b = self.hash[k];
                while b != NONE {
                    let next = self.pool[b as usize].next;
                    if self.pool[b as usize].overlaps(lo, hi) {
                        let flushes = self.stats.flushes;
                        self.drop_block(w, b);
                        if self.stats.flushes != flushes {
                            return;
                        }
                    }
                    b = next;
                }
            }
            return;
        }
        loop {
            let b = self.find_overlap(lo, hi);
            if b == NONE {
                break;
            }
            let flushes = self.stats.flushes;
            self.drop_block(w, b);
            if self.stats.flushes != flushes {
                return;
            }
        }
    }

    // ---- Blocks ----

    fn lookup(&self, pc: u32) -> u32 {
        let mut b = self.hash[bucket_of(pc)];
        while b != NONE {
            if self.pool[b as usize].pc == pc {
                return b;
            }
            b = self.pool[b as usize].next;
        }
        NONE
    }

    fn new_block(&mut self) -> u32 {
        let b = if self.free_list != NONE {
            let b = self.free_list;
            self.free_list = self.pool[b as usize].next;
            b
        } else {
            if self.pool.len() >= POOL_SIZE {
                return NONE;
            }
            self.pool.push(EMPTY_BLOCK);
            (self.pool.len() - 1) as u32
        };
        self.pool[b as usize] = EMPTY_BLOCK;
        b
    }

    /// Decodes the block at `pc` into `insns`; returns whether all its
    /// pages are ROM.
    fn form_block(&mut self, cx: &CpuInfo, w: &Window, pc: u32, max: usize) -> bool {
        let ctx = Ctx {
            mem: w,
            addr_mask: cx.addr_mask,
            model: cx.model,
            fpu: cx.fpu,
            align_check: cx.align_check,
            legal: &self.legal,
        };
        let start = pc & cx.addr_mask;
        let mut at = pc;
        let mut regs: u16 = 0;
        let mut rom = true;
        self.insns.clear();
        while self.insns.len() < max {
            let mut r = rom;
            let Some(i) = decode::decode(&ctx, at, &mut r) else {
                break;
            };
            if u64::from(at & cx.addr_mask) != u64::from(start) + u64::from(at.wrapping_sub(pc)) {
                break;
            }
            if (regs | i.regs).count_ones() as usize > HOST_REGS {
                break;
            }
            regs |= i.regs;
            rom = r;
            at = at.wrapping_add(i.len);
            self.insns.push(i);
            if i.ends_block {
                break;
            }
        }
        rom
    }

    /// Translates the block at `pc` with at most `max` instructions and
    /// puts it in the cache. Returns NONE only when out of descriptors or
    /// code memory (the caller flushes and tries again).
    fn translate(&mut self, cx: &CpuInfo, w: &mut Window, pc: u32, max: usize) -> u32 {
        let rom = self.form_block(cx, w, pc, max);
        let n = self.insns.len();
        let b = self.new_block();
        if b == NONE {
            return NONE;
        }
        let start = pc & cx.addr_mask;
        let mut blk = Block {
            pc,
            start,
            ..EMPTY_BLOCK
        };
        if n == 0 {
            // Not translated: remember that, for the bytes that decided it
            // (at most an instruction's).
            let kind = w.kind(start);
            let room = PAGE_SIZE - (start & (PAGE_SIZE - 1));
            blk.len = if direct_read(kind) { room.min(22) } else { 0 };
            blk.rom = kind == PAGE_ROM;
        } else {
            let last = self.insns[n - 1];
            blk.len = last.pc.wrapping_add(last.len).wrapping_sub(pc);
            blk.ninsns = n as u16;
            blk.cut = n == max && !last.ends_block && max < MAX_INSNS;
            blk.rom = rom;
            let base = self.used;
            let Some(len) = self.tr.translate(&self.insns, base) else {
                self.free_block(b);
                return NONE;
            };
            if len > self.mem.size() - base {
                self.free_block(b);
                return NONE;
            }
            if !self.write_begin() {
                self.free_block(b);
                return NONE;
            }
            // (the translator's buffer, copied into the code memory)
            let Jit { mem, tr, .. } = self;
            mem.write(base, tr.code());
            self.write_end();
            self.used = (base + len + 15) & !15;
            blk.code = Some(base);
            self.stats.blocks += 1;
            self.stats.insns += n as u64;
        }
        let k = bucket_of(pc);
        blk.next = self.hash[k];
        self.pool[b as usize] = blk;
        self.hash[k] = b;
        self.mark_block(w, b);
        b
    }

    /// The block to run at the PC, or None for the interpreter.
    fn get_block(&mut self, cx: &CpuInfo, w: &mut Window, pc: u32, budget: u64) -> Option<u32> {
        let mut b = self.lookup(pc);
        if b != NONE {
            let blk = &self.pool[b as usize];
            if blk.cut && budget > u64::from(blk.ninsns) {
                self.drop_block(w, b);
                b = NONE;
            }
        }
        if b == NONE {
            let max = budget.min(MAX_INSNS as u64) as usize;
            b = self.translate(cx, w, pc, max);
            if b == NONE {
                self.flush(w);
                b = self.translate(cx, w, pc, max);
            }
        }
        if b == NONE {
            return None;
        }
        let blk = &self.pool[b as usize];
        if blk.code.is_none() || u64::from(blk.ninsns) > budget {
            return None;
        }
        Some(b)
    }

    fn set_env(&mut self, w: &mut Window, movbe: bool, lazy: bool) {
        self.env.movbe = movbe && self.has_movbe;
        self.env.lazy = lazy && self.has_lahf;
        self.tr.set_env(self.env);
        self.flush(w);
    }
}

/// What the JIT needs to know of the CPU while it translates.
struct CpuInfo {
    addr_mask: u32,
    model: Model,
    fpu: Fpu,
    align_check: bool,
}

impl Cpu {
    fn cpu_info(&self) -> CpuInfo {
        CpuInfo {
            addr_mask: self.addr_mask,
            model: self.model,
            fpu: self.fpu,
            align_check: self.align_check,
        }
    }

    /// Sets up the JIT engine. Returns false if there is no executable
    /// memory.
    pub fn jit_create(&mut self, legal: Box<[bool]>) -> bool {
        let Some(mut j) = Jit::new(self, legal) else {
            return false;
        };
        j.rebuild_map(&mut self.mem);
        self.jit_map = j.map.as_ptr();
        self.jit_ind = j.ind.as_ptr();
        self.jit = Some(j);
        true
    }

    /// Drops every translation (the page map changed).
    pub fn jit_flush(&mut self) {
        if let Some(j) = self.jit.as_deref_mut() {
            j.flush(&mut self.mem);
        }
    }

    /// Drops the translations of code in [address, address + size).
    pub fn jit_invalidate(&mut self, address: u32, size: u32) {
        let Some(j) = self.jit.as_deref_mut() else {
            return;
        };
        if size == 0 {
            return;
        }
        let lo = u64::from(address & self.addr_mask);
        j.invalidate_range(&mut self.mem, lo, lo + u64::from(size));
    }

    /// `size` bytes at the masked address `addr` of a page with code were
    /// written (memory.rs).
    pub fn jit_code_written(&mut self, addr: u32, size: u32) {
        let Some(j) = self.jit.as_deref_mut() else {
            return;
        };
        let lo = u64::from(addr);
        let hi = lo + u64::from(size);
        if j.may_hit_code(lo, hi) {
            j.invalidate_range(&mut self.mem, lo, hi);
        }
    }

    /// Called by translated code (through the check stub) for an access
    /// the map does not clear at once: 1 if `size` bytes at the masked
    /// address `addr` can be read (or written, bit 16 of `size_write`)
    /// directly in the memory block, which for a write means RAM with no
    /// translated code; 0 if the instruction must be left to the
    /// interpreter (a missing page, a write into translated code); 2 for an
    /// access that is not plain but cannot fault (a host page, a ROM
    /// write), which the block makes through the host step.
    pub fn jit_access_check(&mut self, addr: u32, size_write: u32) -> u32 {
        let size = size_write & 0xFFFF;
        let write = size_write >> 16 != 0;
        let lo = u64::from(addr);
        let hi = lo + u64::from(size);
        if hi - 1 > u64::from(self.addr_mask) {
            return 0;
        }
        let mut direct = true;
        for p in lo >> PAGE_SHIFT..=(hi - 1) >> PAGE_SHIFT {
            let kind = self.mem.page_kind(p as usize);
            if kind == PAGE_NONE {
                return 0;
            }
            let ok = if write {
                kind == PAGE_RAM || kind == PAGE_RAM_CODE
            } else {
                direct_read(kind)
            };
            if !ok {
                direct = false;
            }
        }
        if !direct {
            return 2;
        }
        if let Some(j) = self.jit.as_deref() {
            if write && j.may_hit_code(lo, hi) && j.find_overlap(lo, hi) != NONE {
                return 0;
            }
        }
        1
    }

    /// Runs the instruction at the PC in the interpreter, for a block that
    /// met a host page (or a ROM write). A fault (a bus or address error
    /// from another operand) has been taken when step() returns it, as in
    /// the run loop. Host functions may change the translations
    /// (`relevo_invalidate()`, `relevo_map()`, writes into code); that takes
    /// effect at once, and the block then leaves at once, through code that
    /// nothing changed (no translation happens before the dispatcher runs
    /// again), so it never runs stale code. Returns false if the block may
    /// go on to `next`, true if it must leave for the dispatcher.
    /// (`may_fault`: the instruction has more than the one operand checked;
    /// only the C version needed to know.)
    pub fn jit_host_step(&mut self, next: u32, _may_fault: bool) -> bool {
        self.jit_ccr_flush();
        let (flushes, dropped) = match self.jit.as_deref() {
            Some(j) => (j.stats.flushes, j.stats.dropped),
            None => (0, 0),
        };
        let fault = match self.step() {
            Ok(()) => {
                self.count -= 1; // the dispatcher counts it, by the budget
                false
            }
            Err(_) => {
                // A group 0 exception ended the instruction (as in run()).
                if self.in_insn {
                    self.in_insn = false;
                    self.budget -= 1;
                }
                true
            }
        };
        let Some(j) = self.jit.as_deref_mut() else {
            return true;
        };
        j.stats.host += 1;
        fault
            || self.exiting
            || self.halted
            || self.r.pc != next
            || j.stats.flushes != flushes
            || j.stats.dropped != dropped
    }

    /// The interpreter's handler of `op`, called from a block for an
    /// instruction that cannot take an exception (Kind::Call).
    pub fn jit_call_handler(&mut self, op: u16) {
        let h = self.table[usize::from(op)];
        let r = h(self, op);
        debug_assert!(r.is_ok(), "a handler called from a block faulted");
    }

    /// Merges the record (`jit_ccr`) into SR and clears it.
    pub fn jit_ccr_flush(&mut self) {
        let r = self.jit_ccr;
        let m = ((r >> 16) as u16) & u16::from(F_NZVC);
        let x = r >> 24;
        let mut sr = self.r.sr;
        if m != 0 {
            let fl = (r >> 8) & 0xFF;
            let v = (((fl >> 7) & 1) << 3 | ((fl >> 6) & 1) << 2 | (r & 1) << 1 | (fl & 1)) as u16;
            sr = (sr & !m) | (v & m);
        }
        if x != 0 {
            sr = (sr & !SR_X) | ((x & 1) << 4) as u16;
        }
        self.r.sr = sr;
        self.jit_ccr = 0;
    }

    /// The block to run at the PC: (its index, the address of its code).
    fn jit_get_block(&mut self) -> Option<(u32, usize)> {
        let cx = self.cpu_info();
        let j = self.jit.as_deref_mut()?;
        let b = j.get_block(&cx, &mut self.mem, self.r.pc, self.budget)?;
        let code = j.pool[b as usize].code?;
        Some((b, j.mem.rx_addr(code)))
    }

    /// The run loop of the JIT engine: like the interpreter's, it runs
    /// until the budget is spent or another reason to stop comes up.
    pub fn jit_run_loop(&mut self) -> Res<Status> {
        let pc0 = self.options & OPT_EXIT_ON_PC_ZERO != 0;
        // A jump to chain to the block at a PC: (site, pc, flushes then).
        let mut link: Option<(u32, u32, u64)> = None;
        while self.budget != 0 {
            self.shared.attention.store(0, Relaxed);
            if self.shared.stop_request.load(Relaxed) {
                self.shared.stop_request.store(false, Relaxed);
                return Ok(Status::HostStop);
            }
            if self.between_needed() && !self.between_insns()? {
                return Ok(Status::Stopped);
            }
            let force = self.jit.as_deref().is_some_and(|j| j.force_interp);
            if !force && self.r.sr & SR_T1 == 0 {
                if let Some((b, code)) = self.jit_get_block() {
                    let Some(j) = self.jit.as_deref_mut() else {
                        unreachable!()
                    };
                    let bpc = j.pool[b as usize].pc;
                    // Never straight into PC 0 when the host waits for it.
                    if !(pc0 && bpc == 0) {
                        if let Some((site, lpc, flushes)) = link {
                            if lpc == bpc && flushes == j.stats.flushes {
                                j.link_block(site, b);
                            }
                        }
                        j.ind[ind_index(bpc)] = Ind {
                            pc: bpc,
                            pad: 0,
                            code,
                        };
                    }
                    link = None;
                    j.stats.runs += 1;
                    let trampoline = j.mem.rx_addr(0);
                    let budget = self.budget;
                    let r = native::enter(self, trampoline, code);
                    self.count += budget - self.budget;
                    if self.jit_ccr != 0 {
                        self.jit_ccr_flush();
                    }
                    let Some(j) = self.jit.as_deref_mut() else {
                        unreachable!()
                    };
                    match r as u32 {
                        EXIT_BAIL => {
                            j.stats.bails += 1;
                            j.force_interp = true;
                            continue;
                        }
                        EXIT_CHAIN => {
                            link = Some(((r >> 32) as u32, self.r.pc, j.stats.flushes));
                        }
                        _ => {}
                    }
                    if self.exiting {
                        return Ok(self.exit);
                    }
                    if self.halted {
                        return Ok(Status::Halted);
                    }
                    if pc0 && self.r.pc == 0 {
                        return Ok(Status::ExitPcZero);
                    }
                    continue;
                }
            }
            link = None;
            if let Some(j) = self.jit.as_deref_mut() {
                j.force_interp = false;
                j.stats.interp += 1;
            }
            self.step()?;
            if self.exiting {
                return Ok(self.exit);
            }
            if pc0 && self.r.pc == 0 {
                return Ok(Status::ExitPcZero);
            }
        }
        Ok(Status::Budget)
    }

    // ---- For the tests ----

    pub fn jit_stats(&self) -> Stats {
        self.jit.as_deref().map_or_else(Stats::default, |j| j.stats)
    }

    /// Uses MOVBE (when the CPU has it) or not; flushes.
    pub fn jit_set_movbe(&mut self, on: bool) {
        if let Some(j) = self.jit.as_deref_mut() {
            let lazy = j.env.lazy;
            j.set_env(&mut self.mem, on, lazy);
        }
    }

    /// Keeps condition codes in x86 form between blocks (when the CPU has
    /// LAHF and SAHF) or not; flushes.
    pub fn jit_set_lazy(&mut self, on: bool) {
        if let Some(j) = self.jit.as_deref_mut() {
            let movbe = j.env.movbe;
            j.set_env(&mut self.mem, movbe, on);
        }
    }

    /// Whether the first instruction at `pc` would be translated.
    pub fn jit_translatable(&self, pc: u32) -> bool {
        let Some(j) = self.jit.as_deref() else {
            return false;
        };
        let ctx = Ctx {
            mem: &self.mem,
            addr_mask: self.addr_mask,
            model: self.model,
            fpu: self.fpu,
            align_check: self.align_check,
            legal: &j.legal,
        };
        let mut rom = true;
        decode::decode(&ctx, pc, &mut rom).is_some()
    }
}

// The record's layout (see translate.rs): N Z V C in the low bits of the
// CCR, as in SR.
const _: () = assert!(F_X as u16 == SR_X);
