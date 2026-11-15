// SPDX-License-Identifier: MPL-2.0
//! Executable memory with W^X: no page is ever writable and executable at
//! once. On Linux the same memory is mapped twice, once read-write (where
//! the translator's bytes are copied) and once read-execute (what runs),
//! through a memfd; if that is not possible, one mapping switches between
//! RW and RX with mprotect() around each write. Windows does the same with
//! a section mapped twice, or VirtualAlloc() and VirtualProtect() (written
//! from the Win32 documentation, as in the C version; not run on Windows
//! yet). Without the `std` feature, or on other systems, there is no code
//! memory and so no JIT.
//!
//! One of the modules with `unsafe`: the system calls and the copies into
//! the mappings. Every write is bounds-checked against the mapping and only
//! allowed between `begin_write()` and `end_write()`.
#![allow(unsafe_code)]

pub struct CodeMem {
    /// where code is written
    rw: *mut u8,
    /// where the same bytes run
    rx: *mut u8,
    size: usize,
    /// two mappings; else the protection is toggled
    dual: bool,
    /// Linux: the memfd, or -1
    #[allow(dead_code)]
    fd: i32,
    /// between begin_write() and end_write()
    writable: bool,
}

impl CodeMem {
    pub fn size(&self) -> usize {
        self.size
    }

    /// The address where the byte at `off` runs.
    pub fn rx_addr(&self, off: usize) -> usize {
        debug_assert!(off < self.size);
        self.rx as usize + off
    }

    /// Copies `bytes` to offset `off`. Only between begin_write() and
    /// end_write(); panics outside the memory.
    pub fn write(&mut self, off: usize, bytes: &[u8]) {
        assert!(self.writable && off <= self.size && bytes.len() <= self.size - off);
        // SAFETY: inside the RW mapping (checked above), which is writable
        // now (dual mapping, or switched to RW by begin_write()); nothing
        // else in Rust refers to these bytes.
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), self.rw.add(off), bytes.len());
        }
    }
}

// ---- Linux and other Unix systems ------------------------------------------------

#[cfg(all(feature = "std", unix))]
mod sys {
    use core::ffi::{c_char, c_int, c_long, c_uint, c_void};

    pub const PROT_READ: c_int = 1;
    pub const PROT_WRITE: c_int = 2;
    pub const PROT_EXEC: c_int = 4;
    pub const MAP_SHARED: c_int = 1;
    pub const MAP_PRIVATE: c_int = 2;
    #[cfg(any(target_os = "linux", target_os = "android"))]
    pub const MAP_ANONYMOUS: c_int = 0x20;
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    pub const MAP_ANONYMOUS: c_int = 0x1000;
    pub const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
    #[cfg(target_os = "linux")]
    pub const MFD_CLOEXEC: c_uint = 1;

    unsafe extern "C" {
        pub fn mmap(
            addr: *mut c_void,
            len: usize,
            prot: c_int,
            flags: c_int,
            fd: c_int,
            off: c_long,
        ) -> *mut c_void;
        pub fn munmap(addr: *mut c_void, len: usize) -> c_int;
        pub fn mprotect(addr: *mut c_void, len: usize, prot: c_int) -> c_int;
        pub fn close(fd: c_int) -> c_int;
        #[cfg(target_os = "linux")]
        pub fn memfd_create(name: *const c_char, flags: c_uint) -> c_int;
        #[cfg(target_os = "linux")]
        pub fn ftruncate(fd: c_int, len: c_long) -> c_int;
    }
}

#[cfg(all(feature = "std", unix))]
impl CodeMem {
    #[cfg(target_os = "linux")]
    fn dual_mapping(size: usize) -> Option<CodeMem> {
        use sys::*;
        // SAFETY: plain system calls; the results are checked, and what
        // was made is undone on failure.
        unsafe {
            let fd = memfd_create(c"relevo-jit".as_ptr(), MFD_CLOEXEC);
            if fd < 0 {
                return None;
            }
            if ftruncate(fd, size as core::ffi::c_long) != 0 {
                close(fd);
                return None;
            }
            let rw = mmap(
                core::ptr::null_mut(),
                size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0,
            );
            let rx = mmap(
                core::ptr::null_mut(),
                size,
                PROT_READ | PROT_EXEC,
                MAP_SHARED,
                fd,
                0,
            );
            if rw != MAP_FAILED && rx != MAP_FAILED {
                return Some(CodeMem {
                    rw: rw.cast(),
                    rx: rx.cast(),
                    size,
                    dual: true,
                    fd,
                    writable: false,
                });
            }
            if rw != MAP_FAILED {
                munmap(rw, size);
            }
            if rx != MAP_FAILED {
                munmap(rx, size);
            }
            close(fd);
            None
        }
    }

    #[cfg(not(target_os = "linux"))]
    fn dual_mapping(_size: usize) -> Option<CodeMem> {
        None
    }

    /// `size` bytes of code memory, or None.
    pub fn new(size: usize) -> Option<CodeMem> {
        if let Some(m) = Self::dual_mapping(size) {
            return Some(m);
        }
        use sys::*;
        // One mapping, RX except while the translator writes.
        // SAFETY: a plain system call, checked.
        let p = unsafe {
            mmap(
                core::ptr::null_mut(),
                size,
                PROT_READ | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        if p == MAP_FAILED {
            return None;
        }
        Some(CodeMem {
            rw: p.cast(),
            rx: p.cast(),
            size,
            dual: false,
            fd: -1,
            writable: false,
        })
    }

    /// Brackets every write: after end_write() the bytes written may run.
    pub fn begin_write(&mut self) -> bool {
        if !self.dual {
            // SAFETY: our own mapping; no code runs while it is RW.
            if unsafe { sys::mprotect(self.rw.cast(), self.size, sys::PROT_READ | sys::PROT_WRITE) }
                != 0
            {
                return false;
            }
        }
        self.writable = true;
        true
    }

    pub fn end_write(&mut self) {
        self.writable = false;
        if !self.dual {
            // SAFETY: our own mapping.
            unsafe {
                sys::mprotect(self.rw.cast(), self.size, sys::PROT_READ | sys::PROT_EXEC);
            }
        }
    }
}

#[cfg(all(feature = "std", unix))]
impl Drop for CodeMem {
    fn drop(&mut self) {
        // SAFETY: the mappings made by new(); nothing runs from them now.
        unsafe {
            sys::munmap(self.rx.cast(), self.size);
            if self.dual {
                sys::munmap(self.rw.cast(), self.size);
                sys::close(self.fd);
            }
        }
    }
}

// ---- Windows -------------------------------------------------------------------------

#[cfg(all(feature = "std", windows))]
mod sys {
    use core::ffi::c_void;

    pub type Handle = *mut c_void;
    pub const PAGE_READWRITE: u32 = 0x04;
    pub const PAGE_EXECUTE_READ: u32 = 0x20;
    pub const PAGE_EXECUTE_READWRITE: u32 = 0x40;
    pub const SEC_COMMIT: u32 = 0x0800_0000;
    pub const FILE_MAP_WRITE: u32 = 0x0002;
    pub const FILE_MAP_READ: u32 = 0x0004;
    pub const FILE_MAP_EXECUTE: u32 = 0x0020;
    pub const MEM_COMMIT: u32 = 0x1000;
    pub const MEM_RESERVE: u32 = 0x2000;
    pub const MEM_RELEASE: u32 = 0x8000;
    pub const INVALID_HANDLE_VALUE: Handle = !0usize as Handle;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        pub fn CreateFileMappingW(
            file: Handle,
            attrs: *mut c_void,
            protect: u32,
            size_high: u32,
            size_low: u32,
            name: *const u16,
        ) -> Handle;
        pub fn MapViewOfFile(
            map: Handle,
            access: u32,
            off_high: u32,
            off_low: u32,
            len: usize,
        ) -> *mut c_void;
        pub fn UnmapViewOfFile(addr: *const c_void) -> i32;
        pub fn CloseHandle(h: Handle) -> i32;
        pub fn VirtualAlloc(addr: *mut c_void, size: usize, kind: u32, protect: u32)
        -> *mut c_void;
        pub fn VirtualFree(addr: *mut c_void, size: usize, kind: u32) -> i32;
        pub fn VirtualProtect(addr: *mut c_void, size: usize, protect: u32, old: *mut u32) -> i32;
        pub fn FlushInstructionCache(process: Handle, addr: *const c_void, size: usize) -> i32;
        pub fn GetCurrentProcess() -> Handle;
    }
}

#[cfg(all(feature = "std", windows))]
impl CodeMem {
    /// `size` bytes of code memory, or None.
    pub fn new(size: usize) -> Option<CodeMem> {
        use sys::*;
        // SAFETY: plain system calls; the results are checked, and what
        // was made is undone on failure.
        unsafe {
            let h = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                core::ptr::null_mut(),
                PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                (size as u64 >> 32) as u32,
                size as u32,
                core::ptr::null(),
            );
            if !h.is_null() {
                let rw = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, size);
                let rx = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, size);
                CloseHandle(h); // the views keep the section
                if !rw.is_null() && !rx.is_null() {
                    return Some(CodeMem {
                        rw: rw.cast(),
                        rx: rx.cast(),
                        size,
                        dual: true,
                        fd: -1,
                        writable: false,
                    });
                }
                if !rw.is_null() {
                    UnmapViewOfFile(rw);
                }
                if !rx.is_null() {
                    UnmapViewOfFile(rx);
                }
            }
            let p = VirtualAlloc(
                core::ptr::null_mut(),
                size,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_EXECUTE_READ,
            );
            if p.is_null() {
                return None;
            }
            Some(CodeMem {
                rw: p.cast(),
                rx: p.cast(),
                size,
                dual: false,
                fd: -1,
                writable: false,
            })
        }
    }

    pub fn begin_write(&mut self) -> bool {
        if !self.dual {
            let mut old = 0;
            // SAFETY: our own memory; no code runs while it is RW.
            if unsafe {
                sys::VirtualProtect(self.rw.cast(), self.size, sys::PAGE_READWRITE, &mut old)
            } == 0
            {
                return false;
            }
        }
        self.writable = true;
        true
    }

    pub fn end_write(&mut self) {
        self.writable = false;
        let mut old = 0;
        // SAFETY: our own memory.
        unsafe {
            if !self.dual {
                sys::VirtualProtect(self.rx.cast(), self.size, sys::PAGE_EXECUTE_READ, &mut old);
            }
            sys::FlushInstructionCache(sys::GetCurrentProcess(), self.rx.cast(), self.size);
        }
    }
}

#[cfg(all(feature = "std", windows))]
impl Drop for CodeMem {
    fn drop(&mut self) {
        // SAFETY: the memory made by new(); nothing runs from it now.
        unsafe {
            if self.dual {
                sys::UnmapViewOfFile(self.rx.cast());
                sys::UnmapViewOfFile(self.rw.cast());
            } else {
                sys::VirtualFree(self.rx.cast(), 0, sys::MEM_RELEASE);
            }
        }
    }
}

// ---- Nothing ---------------------------------------------------------------------------

#[cfg(not(all(feature = "std", any(unix, windows))))]
impl CodeMem {
    pub fn new(_size: usize) -> Option<CodeMem> {
        None
    }

    pub fn begin_write(&mut self) -> bool {
        false
    }

    pub fn end_write(&mut self) {}
}
