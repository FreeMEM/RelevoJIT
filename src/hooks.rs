// SPDX-License-Identifier: MPL-2.0
//! Test hooks of the C tests: tests/test_fpu.c calls the extended
//! arithmetic (`fpx_*`, declared in tests/hooks.h) directly, with the C
//! signatures the C version had. Not part of the API.
#![allow(unsafe_code)]
#![allow(non_camel_case_types)]

use core::ffi::{c_int, c_uint};

use crate::fpx::{self, Env, Fpx, Func};

/// `fpx_env`
#[repr(C)]
pub struct fpx_env {
    pub mode: c_uint,
    pub prec: c_uint,
    pub flags: c_uint,
}

/// Runs `f` with the environment behind `e`, and writes its flags back.
///
/// # Safety
///
/// `e` points to a valid `fpx_env`.
unsafe fn with_env<T>(e: *mut fpx_env, f: impl FnOnce(&mut Env) -> T) -> T {
    // SAFETY: the caller's promise.
    let ce = unsafe { &mut *e };
    let mut env = Env {
        mode: ce.mode,
        prec: ce.prec,
        flags: ce.flags,
    };
    let r = f(&mut env);
    ce.flags = env.flags;
    r
}

macro_rules! binary {
    ($($name:ident => $f:path),* $(,)?) => {$(
        /// # Safety
        ///
        /// `e` points to a valid `fpx_env`.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(e: *mut fpx_env, a: Fpx, b: Fpx) -> Fpx {
            // SAFETY: the caller's promise.
            unsafe { with_env(e, |env| $f(env, a, b)) }
        }
    )*};
}

macro_rules! unary {
    ($($name:ident => $f:path),* $(,)?) => {$(
        /// # Safety
        ///
        /// `e` points to a valid `fpx_env`.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(e: *mut fpx_env, a: Fpx) -> Fpx {
            // SAFETY: the caller's promise.
            unsafe { with_env(e, |env| $f(env, a)) }
        }
    )*};
}

binary! {
    fpx_add => fpx::add,
    fpx_sub => fpx::sub,
    fpx_mul => fpx::mul,
    fpx_div => fpx::div,
    fpx_scale => fpx::scale,
}

unary! {
    fpx_sqrt => fpx::sqrt,
    fpx_round => fpx::round,
    fpx_abs => fpx::abs,
    fpx_neg => fpx::neg,
    fpx_getexp => fpx::getexp,
    fpx_getman => fpx::getman,
    fpx_to_extended => fpx::to_extended,
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_int(e: *mut fpx_env, a: Fpx, mode: c_uint) -> Fpx {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::int(env, a, mode)) }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`, `quotient` to a byte.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_rem(
    e: *mut fpx_env,
    a: Fpx,
    b: Fpx,
    ieee: bool,
    quotient: *mut u8,
) -> Fpx {
    // SAFETY: the caller's promise.
    let (r, q) = unsafe { with_env(e, |env| fpx::rem(env, a, b, ieee)) };
    // SAFETY: the caller's promise.
    unsafe { quotient.write(q) };
    r
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_cmp(e: *mut fpx_env, a: Fpx, b: Fpx) -> u32 {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::cmp(env, a, b)) }
}

#[unsafe(no_mangle)]
pub extern "C" fn fpx_cc(a: Fpx) -> u32 {
    fpx::cc(a)
}

#[unsafe(no_mangle)]
pub extern "C" fn fpx_accrued(flags: c_uint) -> c_uint {
    fpx::accrued(flags)
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_from_int(e: *mut fpx_env, v: i32) -> Fpx {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::from_int(env, v)) }
}

#[unsafe(no_mangle)]
pub extern "C" fn fpx_from_single(v: u32) -> Fpx {
    fpx::from_single(v)
}

#[unsafe(no_mangle)]
pub extern "C" fn fpx_from_double(v: u64) -> Fpx {
    fpx::from_double(v)
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_to_int(e: *mut fpx_env, a: Fpx, size: c_uint) -> i32 {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::to_int(env, a, size)) }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_to_single(e: *mut fpx_env, a: Fpx) -> u32 {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::to_single(env, a)) }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_to_double(e: *mut fpx_env, a: Fpx) -> u64 {
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::to_double(env, a)) }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`, `p` to three longs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_from_packed(e: *mut fpx_env, p: *const u32) -> Fpx {
    // SAFETY: the caller's promise.
    let p = unsafe { &*p.cast::<[u32; 3]>() };
    // SAFETY: the caller's promise.
    unsafe { with_env(e, |env| fpx::from_packed(env, p)) }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`, `p` to three longs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_to_packed(e: *mut fpx_env, a: Fpx, k: c_int, p: *mut u32) {
    // SAFETY: the caller's promise.
    let r = unsafe { with_env(e, |env| fpx::to_packed(env, a, k)) };
    // SAFETY: the caller's promise.
    unsafe { p.cast::<[u32; 3]>().write(r) };
}

/// # Safety
///
/// `e` points to a valid `fpx_env`, `out` to an `fpx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_constant(e: *mut fpx_env, offset: c_uint, out: *mut Fpx) -> bool {
    // SAFETY: the caller's promise.
    match unsafe { with_env(e, |env| fpx::constant(env, offset)) } {
        Some(v) => {
            // SAFETY: the caller's promise.
            unsafe { out.write(v) };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `e` points to a valid `fpx_env`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpx_trans(e: *mut fpx_env, f: c_uint, a: Fpx) -> Fpx {
    match Func::from_u32(f) {
        // SAFETY: the caller's promise.
        Some(func) => unsafe { with_env(e, |env| fpx::trans(env, func, a)) },
        None => {
            // SAFETY: the caller's promise.
            unsafe { with_env(e, |env| env.flags |= fpx::OPERR) };
            fpx::default_nan()
        }
    }
}
