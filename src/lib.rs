// SPDX-License-Identifier: MPL-2.0
//! RelevoJIT: a Motorola 680x0 interpreter (and, later, an x86-64 JIT).
//!
//! The crate exports the C interface of `include/relevo.h` (module [`capi`]),
//! with the same ABI as the C version in `c/`. It is `no_std` with `alloc`;
//! the `std` feature (on by default) is for hosted systems and the tests.
//!
//! What is here so far (docs/REESCRITURA.md): the interpreter of the 68000
//! to 68060 without MMU, with its exceptions and cycle counts (RR1), and the
//! FPU with its own extended arithmetic (`fpx`, RR2). The JIT (RR3) comes
//! next; `relevo_create()` returns NULL for it.
//!
//! `unsafe` lives only in two modules: [`capi`] (the C boundary) and
//! `window` (the 68k memory the host gives). Everything else denies it.
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
mod interp;
mod memory;
mod window;

#[cfg(test)]
mod tests;
