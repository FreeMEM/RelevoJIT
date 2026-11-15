// SPDX-License-Identifier: MPL-2.0
//! Into and out of translated code: the call of the trampoline, the
//! functions translated code calls back (System V convention on every
//! system), and CPUID.
//!
//! One of the modules with `unsafe`. Its invariant: translated code only
//! runs inside `enter()`, which hands it the only live reference to the
//! CPU, as a raw pointer; each callback turns that pointer back into a
//! reference for as long as it runs, and translated code passes no other
//! pointer. The code itself is trusted: it comes from the translator, and
//! the dispatcher only enters blocks it has in its cache (dropped when the
//! code memory is reused).
#![allow(unsafe_code)]

use crate::cpu::Cpu;

/// Whether this machine can run the JIT's code.
pub fn available() -> bool {
    cfg!(target_arch = "x86_64")
}

#[cfg(target_arch = "x86_64")]
fn cpuid(leaf: u32) -> (u32, u32, u32, u32) {
    // SAFETY: CPUID exists on every x86-64 processor.
    #[allow(unused_unsafe)]
    let r = unsafe { core::arch::x86_64::__cpuid(leaf) };
    (r.eax, r.ebx, r.ecx, r.edx)
}

/// MOVBE (CPUID 1, ECX bit 22).
pub fn has_movbe() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        cpuid(1).2 >> 22 & 1 != 0
    }
    #[cfg(not(target_arch = "x86_64"))]
    false
}

/// LAHF and SAHF in 64-bit mode (CPUID 0x80000001, ECX bit 0).
pub fn has_lahf() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        if cpuid(0x8000_0000).0 < 0x8000_0001 {
            return false;
        }
        cpuid(0x8000_0001).2 & 1 != 0
    }
    #[cfg(not(target_arch = "x86_64"))]
    false
}

/// Runs translated code: the trampoline at `trampoline` with the block at
/// `code` (both addresses in the code memory, from the dispatcher), until
/// it returns to the dispatcher. Returns the exit code and the chained
/// jump (see translate.rs).
#[cfg(target_arch = "x86_64")]
pub fn enter(cpu: &mut Cpu, trampoline: usize, code: usize) -> u64 {
    type Enter = extern "sysv64" fn(*mut Cpu, *mut u8, usize) -> u64;
    let mem = cpu.mem.base();
    let p: *mut Cpu = cpu;
    // SAFETY: the trampoline is the code jit_create() wrote at the start
    // of the code memory, with this signature; `code` is a translated
    // block of this CPU. From here until it returns, `p` is the only way
    // to the CPU (the module's invariant).
    unsafe {
        let f: Enter = core::mem::transmute::<usize, Enter>(trampoline);
        f(core::hint::black_box(p), mem, code)
    }
}

#[cfg(not(target_arch = "x86_64"))]
pub fn enter(_cpu: &mut Cpu, _trampoline: usize, _code: usize) -> u64 {
    unreachable!("no JIT on this machine")
}

/// The addresses translated code calls.
pub struct Callbacks {
    pub access_check: u64,
    pub host_step: u64,
    pub call_handler: u64,
    pub fpu_op: u64,
    pub fpu_branch: u64,
}

/// The functions translated code calls back, with the System V convention
/// on every system (so the generator has one convention to follow).
#[cfg(target_arch = "x86_64")]
mod calls {
    use super::Callbacks;
    use crate::cpu::Cpu;

    /// The CPU translated code runs for.
    ///
    /// # Safety
    ///
    /// `cpu` is the pointer enter() gave translated code (see the module).
    unsafe fn cpu_of<'a>(cpu: *mut Cpu) -> &'a mut Cpu {
        // SAFETY: the caller's promise.
        unsafe { &mut *cpu }
    }

    extern "sysv64" fn access_check(cpu: *mut Cpu, addr: u32, size_write: u32) -> u32 {
        // SAFETY: called by translated code with its CPU.
        unsafe { cpu_of(cpu) }.jit_access_check(addr, size_write)
    }

    extern "sysv64" fn host_step(cpu: *mut Cpu, next: u32, may_fault: u32) -> u32 {
        // SAFETY: called by translated code with its CPU.
        u32::from(unsafe { cpu_of(cpu) }.jit_host_step(next, may_fault != 0))
    }

    extern "sysv64" fn call_handler(cpu: *mut Cpu, op: u32) {
        // SAFETY: called by translated code with its CPU.
        unsafe { cpu_of(cpu) }.jit_call_handler(op as u16);
    }

    extern "sysv64" fn fpu_op(cpu: *mut Cpu, pc: u32, cmd_ea: u32, value: u32) -> u32 {
        // SAFETY: called by translated code with its CPU.
        unsafe { cpu_of(cpu) }.fpu_jit_op(pc, cmd_ea, value)
    }

    extern "sysv64" fn fpu_branch(cpu: *mut Cpu, pc: u32, pred: u32) -> u32 {
        // SAFETY: called by translated code with its CPU.
        u32::from(unsafe { cpu_of(cpu) }.fpu_jit_branch(pc, pred))
    }

    pub fn callbacks() -> Callbacks {
        type F3 = extern "sysv64" fn(*mut Cpu, u32, u32) -> u32;
        type F2 = extern "sysv64" fn(*mut Cpu, u32);
        type F4 = extern "sysv64" fn(*mut Cpu, u32, u32, u32) -> u32;
        Callbacks {
            access_check: access_check as F3 as usize as u64,
            host_step: host_step as F3 as usize as u64,
            call_handler: call_handler as F2 as usize as u64,
            fpu_op: fpu_op as F4 as usize as u64,
            fpu_branch: fpu_branch as F3 as usize as u64,
        }
    }
}

#[cfg(target_arch = "x86_64")]
pub use calls::callbacks;

/// No translated code runs on other machines (available() is false).
#[cfg(not(target_arch = "x86_64"))]
pub fn callbacks() -> Callbacks {
    Callbacks {
        access_check: 0,
        host_step: 0,
        call_handler: 0,
        fpu_op: 0,
        fpu_branch: 0,
    }
}
