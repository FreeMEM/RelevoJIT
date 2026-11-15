// SPDX-License-Identifier: MPL-2.0
//! The C interface, `include/relevo.h`, with its exact ABI; and the calls
//! into the host's functions. One of the two modules with `unsafe`.
//!
//! A `relevo_cpu *` is a [`Handle`]: the [`Shared`] part that other threads
//! may touch, and the [`Cpu`] that belongs to the thread running it, next
//! to each other (translated code reaches the attention flag from the CPU's
//! address). Each entry point makes a fresh reference from the pointer it
//! gets.
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
use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{align_of, offset_of, size_of};
use core::sync::atomic::Ordering::Relaxed;

use crate::cpu::{Config, Cpu, FpReg, Fpu, Model, Regs, Shared, Status};
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

/// `relevo_code_memory`: the JIT's executable memory, given by the host
/// (see relevo.h). `map` is required; the others are optional.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct relevo_code_memory {
    pub ctx: *mut c_void,
    pub size: usize,
    pub map: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            size: usize,
            rw: *mut *mut c_void,
            rx: *mut *mut c_void,
        ) -> c_int,
    >,
    pub unmap: Option<
        unsafe extern "C" fn(ctx: *mut c_void, rw: *mut c_void, rx: *mut c_void, size: usize),
    >,
    pub begin_write: Option<unsafe extern "C" fn(ctx: *mut c_void) -> c_int>,
    pub end_write: Option<unsafe extern "C" fn(ctx: *mut c_void, offset: usize, size: usize)>,
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
    pub code_memory: *const relevo_code_memory,
}

#[repr(C)]
pub struct Handle {
    shared: Shared,
    cpu: UnsafeCell<Cpu>,
}

/// Where `Shared::attention` is from the CPU (translated code reads it).
pub const ATTENTION_FROM_CPU: i32 = (offset_of!(Handle, shared) + offset_of!(Shared, attention))
    as i32
    - offset_of!(Handle, cpu) as i32;

/// The shared part of the handle a CPU lives in, as the CPU sees it. The
/// CPU is inside the handle, so the reference stays valid as long as the
/// CPU; others only touch the shared part through `&Shared` (atomics).
pub struct SharedRef(*const Shared);

impl SharedRef {
    /// A placeholder, until relevo_create() points it at the handle. Never
    /// dereferenced before that.
    pub fn unset() -> SharedRef {
        SharedRef(core::ptr::null())
    }
}

impl core::ops::Deref for SharedRef {
    type Target = Shared;

    #[inline(always)]
    fn deref(&self) -> &Shared {
        debug_assert!(!self.0.is_null());
        // SAFETY: set by relevo_create() to the `shared` field of the
        // handle that holds this CPU (see above).
        unsafe { &*self.0 }
    }
}

// ---- Layout --------------------------------------------------------------

/// Sizes and field offsets of the structures of relevo.h, as laid out here.
/// tests/abi.c compares them with the C compiler's (`make test`).
const LAYOUT: &[(&str, usize)] = {
    type H = relevo_host;
    type C = relevo_config;
    type K = relevo_code_memory;
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
        ("relevo_config.code_memory", offset_of!(C, code_memory)),
        ("sizeof relevo_code_memory", size_of::<K>()),
        ("alignof relevo_code_memory", align_of::<K>()),
        ("relevo_code_memory.ctx", offset_of!(K, ctx)),
        ("relevo_code_memory.size", offset_of!(K, size)),
        ("relevo_code_memory.map", offset_of!(K, map)),
        ("relevo_code_memory.unmap", offset_of!(K, unmap)),
        ("relevo_code_memory.begin_write", offset_of!(K, begin_write)),
        ("relevo_code_memory.end_write", offset_of!(K, end_write)),
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
    assert!(size_of::<relevo_config>() == 96);
    assert!(offset_of!(relevo_config, memory) == 16);
    assert!(offset_of!(relevo_config, memory_size) == 24);
    assert!(offset_of!(relevo_config, address_mask) == 32);
    assert!(offset_of!(relevo_config, host) == 40);
    assert!(offset_of!(relevo_config, code_memory) == 88);
    assert!(size_of::<relevo_code_memory>() == 48);
    assert!(offset_of!(relevo_code_memory, end_write) == 40);
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

fn fpu_from(raw: c_uint) -> Option<Fpu> {
    Some(match raw {
        0 => Fpu::None,
        1 => Fpu::M68881,
        2 => Fpu::M68882,
        3 => Fpu::Internal,
        _ => return None,
    })
}

/// A host's code memory as relevo.h asks: `map` given, and a size of 0 or
/// a multiple of 4 KB from 64 KB to 1 GB (the JIT's jumps inside it are
/// 32-bit).
fn code_memory_valid(m: &relevo_code_memory) -> bool {
    m.map.is_some()
        && (m.size == 0 || (m.size % 4096 == 0 && (1 << 16..=1 << 30).contains(&m.size)))
}

/// The checks of the C version, and then what this version runs so far.
fn config_valid(cfg: &relevo_config) -> Option<(Model, Fpu)> {
    let model = model_from(cfg.model)?;
    let fpu = fpu_from(cfg.fpu)?;
    if cfg.engine > 1 {
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
    if !fpu_ok || (cfg.engine == 1 && !crate::jit::available()) {
        return None;
    }
    Some((model, fpu))
}

/// # Safety
///
/// `config` is NULL or points to a valid `relevo_config`, whose memory
/// block stays valid until `relevo_destroy()`; its `code_memory` is NULL
/// or points to a valid `relevo_code_memory`, whose functions may be
/// called with its ctx until `relevo_destroy()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_create(config: *const relevo_config) -> *mut relevo_cpu {
    if config.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller's promise.
    let cfg = unsafe { &*config };
    let Some((model, fpu)) = config_valid(cfg) else {
        return core::ptr::null_mut();
    };
    let code_memory = if cfg.engine == 1 && !cfg.code_memory.is_null() {
        // SAFETY: the caller's promise; copied here.
        let m = unsafe { *cfg.code_memory };
        if !code_memory_valid(&m) {
            return core::ptr::null_mut();
        }
        Some(m)
    } else {
        None
    };
    let config = Config {
        model,
        fpu,
        memory: cfg.memory,
        memory_size: cfg.memory_size as u64,
        address_mask: cfg.address_mask,
        options: cfg.options,
        host: cfg.host,
        jit: cfg.engine == 1,
        code_memory,
    };
    // SAFETY: the host's memory block, as relevo.h asks of it.
    let window = unsafe { Window::new(config.memory, config.memory_size) };
    let Some(cpu) = Cpu::new(&config, window, SharedRef::unset()) else {
        return core::ptr::null_mut();
    };
    let handle = Box::into_raw(Box::new(Handle {
        shared: Shared::default(),
        cpu: UnsafeCell::new(cpu),
    }));
    // SAFETY: just made; nobody else has it yet.
    unsafe {
        let c = cpu_mut(handle);
        c.handle = handle;
        c.shared = SharedRef(&raw const (*handle).shared);
    }
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
    if kind > u32::from(crate::window::PAGE_HOST) || !c.map(address, size, kind as u8) {
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

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_invalidate(cpu: *mut relevo_cpu, address: u32, size: u32) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.invalidate(address, size);
}

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
// The tests in tests/ call a few internal functions (tests/hooks.h, as the
// C version had them) and reach into the CPU for the FPU's state. Not part
// of the API.

/// `jit_stats` of tests/hooks.h
pub type jit_stats = crate::jit::Stats;

#[unsafe(no_mangle)]
pub extern "C" fn jit_available() -> bool {
    crate::jit::available()
}

/// # Safety
///
/// `cpu` as for `relevo_map()`; `out` points to a `jit_stats`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_get_stats(cpu: *const relevo_cpu, out: *mut jit_stats) {
    // SAFETY: the caller's promise.
    unsafe { out.write(cpu_mut(cpu.cast_mut()).jit_stats()) };
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_set_movbe(cpu: *mut relevo_cpu, on: bool) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.jit_set_movbe(on);
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_set_lazy(cpu: *mut relevo_cpu, on: bool) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.jit_set_lazy(on);
}

/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_translatable(cpu: *mut relevo_cpu, pc: u32) -> bool {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.jit_translatable(pc)
}

/// The FPU's hidden state: a pending exception's vector (0: none) and
/// whether FSAVE gives a null frame. Reads it into `pending` and `null`
/// (unless NULL), then sets it from `set_pending` and `set_null` if `set`
/// is not 0. For jitdiff and sysdiff.
///
/// # Safety
///
/// See `relevo_map()`; `pending` and `null` are NULL or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn relevo_test_fpu_state(
    cpu: *mut relevo_cpu,
    pending: *mut c_uint,
    null: *mut bool,
    set: c_int,
    set_pending: c_uint,
    set_null: bool,
) {
    // SAFETY: the caller's promise.
    let c = unsafe { cpu_mut(cpu) };
    // SAFETY: the caller's promise.
    unsafe {
        if !pending.is_null() {
            pending.write(c.fpu_pending);
        }
        if !null.is_null() {
            null.write(c.fpu_null);
        }
    }
    if set != 0 {
        c.fpu_pending = set_pending;
        c.fpu_null = set_null;
    }
}

/// Builds the opcode table for `model` and returns how many opcodes two
/// groups claimed (0 unless there is a bug). The table stays this
/// library's: `table` is not written.
#[unsafe(no_mangle)]
pub extern "C" fn interp_fill_table(_table: *mut c_void, model: c_uint, fpu: c_uint) -> c_uint {
    match (model_from(model), fpu_from(fpu)) {
        (Some(m), Some(f)) => crate::interp::build_table(m, f).1,
        _ => 0,
    }
}

/// `cache_write_cacr()` of tests/hooks.h: a write to CACR, as MOVEC does it
/// (tests/test_020.c).
///
/// # Safety
///
/// See `relevo_map()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cache_write_cacr(cpu: *mut relevo_cpu, value: u32) {
    // SAFETY: the caller's promise.
    unsafe { cpu_mut(cpu) }.write_cacr(value);
}
