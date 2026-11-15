// SPDX-License-Identifier: MPL-2.0
//! RelevoJIT: a Motorola 680x0 interpreter and x86-64 JIT.
//!
//! The crate exports the C interface of `include/relevo.h` (module [`capi`]),
//! with the same ABI as the C version in `c/`. It is `no_std` with `alloc`;
//! the `std` feature (on by default) is for hosted systems and the tests.
//!
//! What is here (docs/REESCRITURA.md): the interpreter of the 68000 to
//! 68060 without MMU, with its exceptions and cycle counts (RR1), the FPU
//! with its own extended arithmetic (`fpx`, RR2), and the JIT for x86-64
//! hosts (`jit`, RR3).
//!
//! `unsafe` lives only in five modules: [`capi`] (the C boundary), `hooks`
//! (internal functions the C tests call, with C signatures), `window` (the
//! 68k memory the host gives), `jit::codemem` (the executable memory) and
//! `jit::native` (the call into translated code and its calls back).
//! Everything else denies it.
#![no_std]
#![deny(unsafe_code)]

extern crate alloc;
#[cfg(feature = "std")]
extern crate std;

mod cache;
pub mod capi;
mod cpu;
mod exception;
mod fpx;
mod hooks;
mod interp;
mod jit;
mod memory;
mod window;

#[cfg(test)]
mod tests;
