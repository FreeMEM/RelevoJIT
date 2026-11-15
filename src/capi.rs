// SPDX-License-Identifier: MPL-2.0
//! The C interface, `include/relevo.h`, with its exact ABI; and the calls
//! into the host's functions. One of the two modules with `unsafe`.
//!
//! A `relevo_cpu *` is a [`Handle`]: the [`Shared`] part that other threads
//! may touch, and the [`Cpu`] that belongs to the thread running it. Each
//! entry point makes a fresh reference from the pointer it gets.
//!
//! The host's functions may call back into the API (`relevo_get_regs()`,
//! `relevo_read()`, `relevo_map()`...) with that pointer while the CPU runs,
//! as the C version allows. Before each call into the host, the CPU's
//! address is passed through `black_box`, so the compiler must assume the
//! host may read and change it; after the call, the code reads the state
//! again.
#![allow(unsafe_code)]
#![allow(non_camel_case_types)]

use alloc::boxed::Box;
use alloc::sync::Arc;
use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{align_of, offset_of, size_of};
use core::sync::atomic::Ordering::Relaxed;

use crate::cpu::{Config, Cpu, FpReg, Model, Regs, Shared, Status};
use crate::window::Window;

// ---- Types ---------------------------------------------------------------

pub type relevo_regs = Regs;
pub type relevo_fpreg = FpReg;
/// What the host knows a CPU as.
pub type relevo_cpu = Handle;

/// `relevo_trap_result`
pub const RELEVO_TRAP_EXCEPTION: c_uint = 0;
pub const RELEVO_TRAP_HANDLED: c_uint = 1;
pub const RELEVO_TRAP_STOP: c_uint = 2;

pub const RELEVO_PAGE_SIZE: u64 = 1 << 16;

/// The host's functions, as `relevo_host`. All are optional.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct relevo_host {
    pub ctx: *mut c_void,
    pub read: Option<unsafe extern "C" fn(ctx: *mut c_void, address: u32, size: c_uint) -> u32>,
    pub write:
        Option<unsafe extern "C" fn(ctx: *mut c_void, address: u32, value: u32, size: c_uint)>,
    pub line_a:
        Option<unsafe extern "C" fn(ctx: *mut c_void, cpu: *mut relevo_cpu, opcode: u16) -> c_uint>,
    pub line_f:
        Option<unsafe extern "C" fn(ctx: *mut c_void, cpu: *mut relevo_cpu, opcode: u16) -> c_uint>,
    pub reset: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
}

/// `relevo_config`. The enums are plain integers here: the host may pass
/// any value, and an unknown one makes `relevo_create()` fail.
#[repr(C)]
pub struct relevo_config {
    pub model: c_uint,
    pub fpu: c_uint,
    pub engine: c_uint,
    pub memory: *mut u8,
    pub memory_size: usize,
    pub address_mask: u32,
    pub options: c_uint,
    pub host: relevo_host,
}

pub struct Handle {
    shared: Arc<Shared>,
    cpu: UnsafeCell<Cpu>,
}

// ---- Layout --------------------------------------------------------------

/// Sizes and field offsets of the structures of relevo.h, as laid out here.
/// tests/abi.c compares them with the C compiler's (`make test`).
const LAYOUT: &[(&str, usize)] = {
    type H = relevo_host;
    type C = relevo_config;
    type F = relevo_fpreg;
    type R = relevo_regs;
    &[
        ("sizeof relevo_model", size_of::<c_uint>()),
        ("sizeof relevo_fpu", size_of::<c_uint>()),
        ("sizeof relevo_engine", size_of::<c_uint>()),
        ("sizeof relevo_trap_result", size_of::<c_uint>()),
        ("sizeof relevo_page_kind", size_of::<c_uint>()),
        ("sizeof relevo_status", size_of::<c_uint>()),
        ("sizeof relevo_host", size_of::<H>()),
        ("alignof relevo_host", align_of::<H>()),
        ("relevo_host.ctx", offset_of!(H, ctx)),
        ("relevo_host.read", offset_of!(H, read)),
        ("relevo_host.write", offset_of!(H, write)),
        ("relevo_host.line_a", offset_of!(H, line_a)),
        ("relevo_host.line_f", offset_of!(H, line_f)),
        ("relevo_host.reset", offset_of!(H, reset)),
        ("sizeof relevo_config", size_of::<C>()),
        ("alignof relevo_config", align_of::<C>()),
        ("relevo_config.model", offset_of!(C, model)),
        ("relevo_config.fpu", offset_of!(C, fpu)),
        ("relevo_config.engine", offset_of!(C, engine)),
        ("relevo_config.memory", offset_of!(C, memory)),
        ("relevo_config.memory_size", offset_of!(C, memory_size)),
        ("relevo_config.address_mask", offset_of!(C, address_mask)),
        ("relevo_config.options", offset_of!(C, options)),
        ("relevo_config.host", offset_of!(C, host)),
        ("sizeof relevo_fpreg", size_of::<F>()),
        ("alignof relevo_fpreg", align_of::<F>()),
        ("relevo_fpreg.mantissa", offset_of!(F, mantissa)),
        ("relevo_fpreg.sign_exponent", offset_of!(F, sign_exponent)),
        ("sizeof relevo_regs", size_of::<R>()),
        ("alignof relevo_regs", align_of::<R>()),
        ("relevo_regs.d", offset_of!(R, d)),
        ("relevo_regs.a", offset_of!(R, a)),
        ("relevo_regs.pc", offset_of!(R, pc)),
        ("relevo_regs.sr", offset_of!(R, sr)),
        ("relevo_regs.usp", offset_of!(R, usp)),
        ("relevo_regs.isp", offset_of!(R, isp)),
        ("relevo_regs.msp", offset_of!(R, msp)),
        ("relevo_regs.vbr", offset_of!(R, vbr)),
        ("relevo_regs.cacr", offset_of!(R, cacr)),
        ("relevo_regs.caar", offset_of!(R, caar)),
        ("relevo_regs.sfc", offset_of!(R, sfc)),
        ("relevo_regs.dfc", offset_of!(R, dfc)),
        ("relevo_regs.fp", offset_of!(R, fp)),
        ("relevo_regs.fpcr", offset_of!(R, fpcr)),
        ("relevo_regs.fpsr", offset_of!(R, fpsr)),
        ("relevo_regs.fpiar", offset_of!(R, fpiar)),
        ("relevo_regs.tc", offset_of!(R, tc)),
        ("relevo_regs.itt0", offset_of!(R, itt0)),
        ("relevo_regs.itt1", offset_of!(R, itt1)),
        ("relevo_regs.dtt0", offset_of!(R, dtt0)),
        ("relevo_regs.dtt1", offset_of!(R, dtt1)),
        ("relevo_regs.mmusr", offset_of!(R, mmusr)),
        ("relevo_regs.urp", offset_of!(R, urp)),
        ("relevo_regs.srp", offset_of!(R, srp)),
        ("relevo_regs.buscr", offset_of!(R, buscr)),
        ("relevo_regs.pcr", offset_of!(R, pcr)),
    ]
};

// The layout on 64-bit systems (System V and Microsoft x64 agree here), so
// that a change shows at compile time; tests/abi.c checks every system.
#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(size_of::<relevo_host>() == 48);
    assert!(size_of::<relevo_config>() == 88);
    assert!(offset_of!(relevo_config, memory) == 16);
    assert!(offset_of!(relevo_config, memory_size) == 24);
    assert!(offset_of!(relevo_config, address_mask) == 32);
    assert!(offset_of!(relevo_config, host) == 40);
    assert!(size_of::<relevo_fpreg>() == 16);
    assert!(size_of::<relevo_regs>() == 288);
    assert!(offset_of!(relevo_regs, pc) == 64);
    assert!(offset_of!(relevo_regs, sr) == 68);
    assert!(offset_of!(relevo_regs, usp) == 72);
    assert!(offset_of!(relevo_regs, fp) == 104);
    assert!(offset_of!(relevo_regs, fpcr) == 232);
    assert!(offset_of!(relevo_regs, pcr) == 280);
};

/// The size or offset named `name` (as tests/abi.c names them), or
/// `u64::MAX`. Not part of the API.
///
/// # Safety
///
/// `name` is a NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_abi_layout(name: *const c_char) -> u64 {
    if name.is_null() {
        return u64::MAX;
    }
    // SAFETY: the caller's promise.
    let name = unsafe { core::ffi::CStr::from_ptr(name) }.to_bytes();
    LAYOUT
        .iter()
        .find(|(n, _)| n.as_bytes() == name)
        .map_or(u64::MAX, |&(_, v)| v as u64)
}

// ---- Calls into the host -------------------------------------------------

/// What the host's line A / line F function asked for.
pub enum TrapResult {
    Exception,
    Handled,
    Stop,
}

impl Cpu {
    /// The CPU's address escapes: the host may reach the CPU through its
    /// handle during the call that follows.
    #[inline(always)]
    fn escape(&mut self) {
        core::hint::black_box(self as *mut Cpu);
    }

    /// A read of a host page; 0 without a read function.
    pub fn host_read(&mut self, address: u32, size: u32) -> u32 {
        let Some(f) = self.host.read else { return 0 };
        let ctx = self.host.ctx;
        self.escape();
        // SAFETY: a function the host gave, called as relevo.h says.
        unsafe { f(ctx, address, size) }
    }

    pub fn host_write(&mut self, address: u32, value: u32, size: u32) {
        let Some(f) = self.host.write else { return };
        let ctx = self.host.ctx;
        self.escape();
        // SAFETY: as host_read().
        unsafe { f(ctx, address, value, size) }
    }

    /// Line A (`line_f` false) or line F; `Exception` without a function.
    pub fn host_line(&mut self, line_f: bool, opcode: u16) -> TrapResult {
        let f = if line_f {
            self.host.line_f
        } else {
            self.host.line_a
        };
        let Some(f) = f else {
            return TrapResult::Exception;
        };
        let ctx = self.host.ctx;
        let handle = self.handle;
        self.escape();
        // SAFETY: as host_read(); the handle is this CPU's.
        match unsafe { f(ctx, handle, opcode) } {
            RELEVO_TRAP_HANDLED => TrapResult::Handled,
            RELEVO_TRAP_STOP => TrapResult::Stop,
            _ => TrapResult::Exception,
        }
    }

    pub fn host_reset(&mut self) {
        let Some(f) = self.host.reset else { return };
        let ctx = self.host.ctx;
        self.escape();
        // SAFETY: as host_read().
        unsafe { f(ctx) }
    }
}

// ---- The API ---------------------------------------------------------------

/// The CPU behind a handle.
///
/// # Safety
///
/// `cpu` comes from `relevo_create()` and was not destroyed; the caller is
/// the thread that runs it (or its host functions).
unsafe fn cpu_mut<'a>(cpu: *mut relevo_cpu) -> &'a mut Cpu {
    // SAFETY: the caller's promise.
    unsafe { &mut *(*cpu).cpu.get() }
}

fn model_from(raw: c_uint) -> Option<Model> {
    Some(match raw {
        0 => Model::M68000,
        1 => Model::M68010,
        2 => Model::M68020,
        3 => Model::M68030,
        4 => Model::M68040,
        5 => Model::M68060,
        _ => return None,
    })
}

/// The checks of the C version, and then what this version runs so far.
fn config_valid(cfg: &relevo_config) -> Option<Model> {
    let model = model_from(cfg.model)?;
    if cfg.fpu > 3 || cfg.engine > 1 {
        return None;
    }
    if cfg.memory_size as u64 % RELEVO_PAGE_SIZE != 0 || cfg.memory_size as u64 > 1 << 32 {
        return None;
    }
    if cfg.memory_size != 0 && cfg.memory.is_null() {
        return None;
    }
    let fpu_ok = match cfg.fpu {
        1 | 2 => matches!(model, Model::M68020 | Model::M68030),
        3 => matches!(model, Model::M68040 | Model::M68060),
        _ => true,
    };
    if !fpu_ok {
        return None;
    }
    // Not yet: the 68010 to 68060 and the FPU (RR2), the JIT (RR3).
    if model != Model::M68000 || cfg.engine != 0 {
        return None;
    }
    Some(model)
}

/// # Safety
///
/// `config` is NULL or points to a valid `relevo_config`, whose memory
/// block stays valid until `relevo_destroy()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_create(config: *const relevo_config) -> *mut relevo_cpu {
    if config.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller's promise.
    let cfg = unsafe { &*config };
    let Some(model) = config_valid(cfg) else {
        return core::ptr::null_mut();
    };
    let shared = Arc::new(Shared::default());
    let config = Config {
        model,
        memory: cfg.memory,
        memory_size: cfg.memory_size as u64,
        address_mask: cfg.address_mask,
        options: cfg.options,
        host: cfg.host,
    };
    // SAFETY: the host's memory block, as relevo.h asks of it.
    let window = unsafe { Window::new(config.memory, config.memory_size) };
    let cpu = Cpu::new(&config, window, Arc::clone(&shared));
    let handle = Box::into_raw(Box::new(Handle {
        shared,
        cpu: UnsafeCell::new(cpu),
    }));
    // SAFETY: just made; nobody else has it yet.
    unsafe { cpu_mut(handle).handle = handle };
    handle
}

/// # Safety
///
/// `cpu` is NULL or comes from `relevo_create()` and was not destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_destroy(cpu: *mut relevo_cpu) {
    if !cpu.is_null() {
        // SAFETY: the caller's promise; made by Box::into_raw().
        drop(unsafe { Box::from_raw(cpu) });
    }
}

/// # Safety
///
/// As for every function here: `cpu` comes from `relevo_create()`, and
/// the caller is the thread that runs it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_map(
    cpu: *mut relevo_cpu,
    address: u32,
    size: u64,
    kind: c_uint,
) -> c_int {
    // SAFETY: the caller's promise.
    let c = unsafe { cpu_mut(cpu) };
    if kind > u32::from(crate::window::PAGE_HOST) || !c.mem.map(address, size, kind as u8) {
        return -1;
    }
    0
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_get_regs(cpu: *mut relevo_cpu) -> *mut relevo_regs {
    // SAFETY: the caller's promise.
    unsafe { &raw mut cpu_mut(cpu).r }
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_set_sr(cpu: *mut relevo_cpu, sr: u16) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.set_sr(sr);
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_reset(cpu: *mut relevo_cpu) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.reset();
}

/// # Safety
///
/// See `relevo_map()`; not from the host's functions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_run(cpu: *mut relevo_cpu, budget: u64) -> c_uint {
    // SAFETY: the caller's promise.
    let st: Status = unsafe { cpu_mut(cpu) }.run(budget);
    st as c_uint
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_instructions(cpu: *const relevo_cpu) -> u64 {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu.cast_mut()) }.count
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_cycles(cpu: *const relevo_cpu) -> u64 {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu.cast_mut()) }.cycles
}

/// # Safety
///
/// `cpu` comes from `relevo_create()`; any thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_set_ipl(cpu: *mut relevo_cpu, level: c_uint) {
    // SAFETY: the caller's promise; only the shared part is touched.
    let shared = unsafe { &(*cpu).shared };
    shared.ipl.store(level & 7, Relaxed);
    shared.attention.store(1, Relaxed);
}

/// # Safety
///
/// `cpu` comes from `relevo_create()`; any thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_request_stop(cpu: *mut relevo_cpu) {
    // SAFETY: the caller's promise; only the shared part is touched.
    let shared = unsafe { &(*cpu).shared };
    shared.stop_request.store(true, Relaxed);
    shared.attention.store(1, Relaxed);
}

/// The interpreter keeps no translated code.
///
/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_invalidate(_cpu: *mut relevo_cpu, _address: u32, _size: u32) {}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_read(cpu: *mut relevo_cpu, address: u32, size: c_uint) -> u32 {
    // SAFETY: the caller's promise.
    let c = unsafe { cpu_mut(cpu) };
    match size {
        1 => c.mem_read8(address),
        2 => c.mem_read16(address),
        4 => c.mem_read32(address),
        _ => 0,
    }
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_write(
    cpu: *mut relevo_cpu,
    address: u32,
    value: u32,
    size: c_uint,
) {
    // SAFETY: the caller's promise.
    let c = unsafe { cpu_mut(cpu) };
    match size {
        1 => c.mem_write8(address, value),
        2 => c.mem_write16(address, value),
        4 => c.mem_write32(address, value),
        _ => {}
    }
}

// ---- Hooks of the C tests ----------------------------------------------------
//
// The tests in tests/ call a few internal functions of the C version
// (c/src/jit/jit.h, interp_fill_table()). These are their stand-ins until
// the pieces they test arrive (the JIT: RR3). Not part of the API.

/// `jit_stats` of c/src/jit/jit.h
#[repr(C)]
pub struct jit_stats {
    pub blocks: u64,
    pub insns: u64,
    pub runs: u64,
    pub bails: u64,
    pub interp: u64,
    pub flushes: u64,
    pub links: u64,
    pub dropped: u64,
    pub host: u64,
}

/// No JIT yet.
#[unsafe(no_mangle)]
pub extern "C" fn jit_available() -> bool {
    false
}

/// # Safety
///
/// `out` points to a `jit_stats`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_get_stats(_cpu: *const relevo_cpu, out: *mut jit_stats) {
    // SAFETY: the caller's promise.
    unsafe {
        out.write(jit_stats {
            blocks: 0,
            insns: 0,
            runs: 0,
            bails: 0,
            interp: 0,
            flushes: 0,
            links: 0,
            dropped: 0,
            host: 0,
        })
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn jit_set_movbe(_cpu: *mut relevo_cpu, _on: bool) {}

#[unsafe(no_mangle)]
pub extern "C" fn jit_set_lazy(_cpu: *mut relevo_cpu, _on: bool) {}

#[unsafe(no_mangle)]
pub extern "C" fn jit_translatable(_cpu: *mut relevo_cpu, _pc: u32) -> bool {
    false
}

/// Builds the opcode table for `model` and returns how many opcodes two
/// groups claimed (0 unless there is a bug). The table stays this
/// library's: `table` is not written.
#[unsafe(no_mangle)]
pub extern "C" fn interp_fill_table(_table: *mut c_void, model: c_uint, _fpu: c_uint) -> c_uint {
    match model_from(model) {
        Some(m) => crate::interp::build_table(m).1,
        None => 0,
    }
}
