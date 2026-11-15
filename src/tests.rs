// SPDX-License-Identifier: MPL-2.0
//! Tests through the C interface, as a host would use it, and a few checks
//! of the crate's own. (The C tests in tests/ cover the rest.)
//!
//! Calling the C interface is `unsafe`; these tests are its only users
//! inside the crate.
#![allow(unsafe_code)]

use alloc::vec;
use alloc::vec::Vec;
use core::ptr::{null, null_mut};

use crate::capi::*;
use crate::cpu::{Model, OPT_EXIT_ON_PC_ZERO, Status};

const MEM_SIZE: usize = 0x40000; // 256 KB: pages 0 to 3

const NOP: u16 = 0x4E71;
const RTS: u16 = 0x4E75;

const BUDGET: c_uint = Status::Budget as c_uint;
const EXIT_PC_ZERO: c_uint = Status::ExitPcZero as c_uint;

use core::ffi::{c_uint, c_void};

/// A host's memory block and a CPU on it.
struct Machine {
    mem: Vec<u8>,
    cpu: *mut relevo_cpu,
}

fn zeroed_config() -> relevo_config {
    relevo_config {
        model: 0,
        fpu: 0,
        engine: 0,
        memory: null_mut(),
        memory_size: 0,
        address_mask: 0,
        options: 0,
        host: relevo_host {
            ctx: null_mut(),
            read: None,
            write: None,
            line_a: None,
            line_f: None,
            reset: None,
        },
    }
}

impl Machine {
    fn with(size: usize, edit: impl FnOnce(&mut relevo_config)) -> Machine {
        let mut mem = vec![0u8; size];
        let mut cfg = zeroed_config();
        cfg.memory = mem.as_mut_ptr();
        cfg.memory_size = size;
        edit(&mut cfg);
        let cpu = unsafe { relevo_create(&cfg) };
        assert!(!cpu.is_null());
        Machine { mem, cpu }
    }

    fn new(options: c_uint) -> Machine {
        Machine::with(MEM_SIZE, |cfg| cfg.options = options)
    }

    /// The live registers. Each call makes a new reference: do not keep two.
    #[allow(clippy::mut_from_ref)]
    fn regs(&self) -> &mut relevo_regs {
        unsafe { &mut *relevo_get_regs(self.cpu) }
    }

    fn put16(&mut self, a: usize, v: u16) {
        self.mem[a..a + 2].copy_from_slice(&v.to_be_bytes());
    }

    fn put32(&mut self, a: usize, v: u32) {
        self.mem[a..a + 4].copy_from_slice(&v.to_be_bytes());
    }

    fn get16(&self, a: usize) -> u32 {
        u32::from(u16::from_be_bytes([self.mem[a], self.mem[a + 1]]))
    }

    fn run(&self, budget: u64) -> c_uint {
        unsafe { relevo_run(self.cpu, budget) }
    }

    fn read(&self, a: u32, size: c_uint) -> u32 {
        unsafe { relevo_read(self.cpu, a, size) }
    }

    fn write(&self, a: u32, v: u32, size: c_uint) {
        unsafe { relevo_write(self.cpu, a, v, size) }
    }

    fn map(&self, a: u32, size: u64, kind: c_uint) -> i32 {
        unsafe { relevo_map(self.cpu, a, size, kind) }
    }
}

impl Drop for Machine {
    fn drop(&mut self) {
        unsafe { relevo_destroy(self.cpu) };
    }
}

#[test]
fn table_no_conflicts() {
    use crate::cpu::Fpu;
    for model in [
        Model::M68000,
        Model::M68010,
        Model::M68020,
        Model::M68030,
        Model::M68040,
        Model::M68060,
    ] {
        for fpu in [Fpu::None, Fpu::M68881, Fpu::M68882, Fpu::Internal] {
            let (_, conflicts) = crate::interp::build_table(model, fpu);
            assert_eq!(conflicts, 0, "{model:?} {fpu:?}");
        }
    }
}

#[test]
fn config_validation() {
    let mut mem = vec![0u8; MEM_SIZE];
    let mut cfg = zeroed_config();
    unsafe {
        let c = relevo_create(&cfg);
        assert!(!c.is_null());
        relevo_destroy(c);
        assert!(relevo_create(null()).is_null());

        cfg.memory = mem.as_mut_ptr();
        cfg.memory_size = 0x1000; // not a whole page
        assert!(relevo_create(&cfg).is_null());
        cfg.memory = null_mut();
        cfg.memory_size = MEM_SIZE; // size without memory
        assert!(relevo_create(&cfg).is_null());
        cfg.memory = mem.as_mut_ptr();
        cfg.engine = 2; // no such engine
        assert!(relevo_create(&cfg).is_null());
        cfg.engine = 0;
        cfg.fpu = 1; // a 68881, not on a 68000
        assert!(relevo_create(&cfg).is_null());
        cfg.fpu = 0;
        cfg.model = 99;
        assert!(relevo_create(&cfg).is_null());

        // Every model, each FPU where it fits.
        for (model, fpu) in [(1, 0), (2, 1), (3, 2), (4, 3), (5, 3), (5, 0)] {
            cfg.model = model;
            cfg.fpu = fpu;
            let c = relevo_create(&cfg);
            assert!(!c.is_null());
            relevo_destroy(c);
        }
        cfg.model = 4;
        cfg.fpu = 1; // a 68881 on a 68040
        assert!(relevo_create(&cfg).is_null());
        cfg.model = 0;
        cfg.fpu = 0;
        // Not yet: the JIT (RR3).
        cfg.engine = 1;
        assert!(relevo_create(&cfg).is_null());
        cfg.engine = 0;
        let c = relevo_create(&cfg);
        assert!(!c.is_null());
        relevo_destroy(c);
        relevo_destroy(null_mut());
    }
}

#[test]
fn map_pages() {
    let mut m = Machine::new(0);
    assert_eq!(m.map(0x8000, 0x10000, 1), -1);
    assert_eq!(m.map(0x10000, 0x8000, 1), -1);
    assert_eq!(m.map(0x30000, 0x20000, 1), -1);
    assert_eq!(m.map(0x40000, 0x10000, 2), -1);
    assert_eq!(m.map(0x20000, 0x20000, 2), 0);
    assert_eq!(m.map(0xDF0000, 0x10000, 3), 0);
    assert_eq!(m.map(0xFFFF_0000, 0x20000, 3), -1);
    assert_eq!(m.map(0, 1 << 32, 0), 0);
    assert_eq!(m.map(0, 0x10000, 7), -1);
    // After unmapping everything, memory is not touched.
    m.mem[0x100] = 0x5A;
    assert_eq!(m.read(0x100, 1), 0);
    m.write(0x100, 0xFF, 1);
    assert_eq!(m.mem[0x100], 0x5A);
}

#[derive(Default)]
struct HostLog {
    writes: Vec<(u32, u32, c_uint)>,
    reads: u32,
}

unsafe extern "C" fn host_read(ctx: *mut c_void, addr: u32, size: c_uint) -> u32 {
    let log = unsafe { &mut *ctx.cast::<HostLog>() };
    log.reads += 1;
    (addr & 0xFF) | (size << 8)
}

unsafe extern "C" fn host_write(ctx: *mut c_void, addr: u32, value: u32, size: c_uint) {
    let log = unsafe { &mut *ctx.cast::<HostLog>() };
    log.writes.push((addr, value, size));
}

#[test]
fn memory_page_crossing() {
    let mut log = HostLog::default();
    let ctx = (&raw mut log).cast::<c_void>();
    let m = Machine::with(MEM_SIZE, |cfg| {
        cfg.host.ctx = ctx;
        cfg.host.read = Some(host_read);
        cfg.host.write = Some(host_write);
    });
    // RAM to RAM
    m.write(0xFFFE, 0xA1B2_C3D4, 4);
    assert_eq!(m.read(0xFFFE, 4), 0xA1B2_C3D4);
    m.write(0x1FFFF, 0x1234, 2);
    assert_eq!(m.read(0x1FFFF, 2), 0x1234);
    // RAM to host: the host sees byte accesses past the boundary.
    assert_eq!(m.map(0x30000, 0x10000, 3), 0);
    m.write(0x2FFFE, 0x1122_3344, 4);
    assert_eq!(m.get16(0x2FFFE), 0x1122);
    let log = unsafe { &*ctx.cast::<HostLog>() };
    assert_eq!(log.writes, [(0x30000, 0x33, 1), (0x30001, 0x44, 1)]);
    // The CPU keeps only the bytes it asked for.
    assert_eq!(m.read(0x2FFFE, 4), 0x1122_0001);
    assert_eq!(m.read(0x30010, 1), 0x10);
    assert_eq!(unsafe { &*ctx.cast::<HostLog>() }.reads, 3);
}

#[test]
fn address_mask() {
    // 68000: 24-bit bus, the top byte is ignored.
    let m = Machine::new(0);
    m.write(0xFF00_0100, 0xCAFE, 2);
    assert_eq!(m.get16(0x100), 0xCAFE);
    assert_eq!(m.read(0x0100_0100, 2), 0xCAFE);
    drop(m);
    // An explicit mask: accesses wrap around the end.
    let m = Machine::with(MEM_SIZE, |cfg| cfg.address_mask = MEM_SIZE as u32 - 1);
    m.write(MEM_SIZE as u32 - 2, 0x5566_7788, 4);
    assert_eq!(m.get16(MEM_SIZE - 2), 0x5566);
    assert_eq!(m.get16(0), 0x7788);
    assert_eq!(m.read(MEM_SIZE as u32 - 2, 4), 0x5566_7788);
}

#[test]
fn rts_to_zero_exits() {
    let mut m = Machine::new(OPT_EXIT_ON_PC_ZERO);
    m.put16(0x1000, NOP);
    m.put16(0x1002, NOP);
    m.put16(0x1004, RTS);
    m.put32(0x8000, 0);
    let r = m.regs();
    r.pc = 0x1000;
    r.a[7] = 0x8000;
    assert_eq!(m.run(100), EXIT_PC_ZERO);
    assert_eq!(m.regs().pc, 0);
    assert_eq!(m.regs().a[7], 0x8004);
    assert_eq!(unsafe { relevo_instructions(m.cpu) }, 3);
}

#[test]
fn set_sr_switches_stacks() {
    let m = Machine::new(0);
    let r = m.regs();
    r.a[7] = 0x5000; // supervisor after create
    r.usp = 0x6000;
    unsafe { relevo_set_sr(m.cpu, 0) };
    assert_eq!((m.regs().a[7], m.regs().isp), (0x6000, 0x5000));
    m.regs().a[7] = 0x6100;
    unsafe { relevo_set_sr(m.cpu, 0x3700) }; // M does not exist on the 68000
    assert_eq!(m.regs().sr, 0x2700);
    assert_eq!((m.regs().a[7], m.regs().usp), (0x5000, 0x6100));
}

#[test]
fn independent_cpus() {
    let mut a = Machine::new(OPT_EXIT_ON_PC_ZERO);
    let mut b = Machine::with(0x10000, |_| {});
    a.put16(0x1000, RTS);
    b.put16(0x1000, NOP);
    a.regs().pc = 0x1000;
    a.regs().a[7] = 0x8000;
    b.regs().pc = 0x1000;
    assert_eq!(a.run(5), EXIT_PC_ZERO);
    assert_eq!(b.run(1), BUDGET);
    assert_eq!(a.regs().pc, 0);
    assert_eq!(b.regs().pc, 0x1002);
    assert_eq!(b.regs().a[7], 0);
}

/// An interrupt from another thread wakes a CPU stopped by STOP.
#[cfg(feature = "std")]
#[test]
fn interrupt_from_another_thread() {
    let mut m = Machine::new(0);
    m.put32(0, 0x8000); // reset SSP
    m.put32(4, 0x1000); // reset PC
    m.put32(4 * 27, 0x2000); // level 3 autovector
    m.put16(0x1000, 0x4E72); // STOP #$2000
    m.put16(0x1002, 0x2000);
    m.put16(0x2000, NOP);
    unsafe { relevo_reset(m.cpu) };
    assert_eq!(m.run(10), Status::Stopped as c_uint);
    let cpu = m.cpu as usize;
    std::thread::spawn(move || unsafe { relevo_set_ipl(cpu as *mut relevo_cpu, 3) })
        .join()
        .unwrap();
    assert_eq!(m.run(1), BUDGET);
    assert_eq!(m.regs().pc, 0x2002);
    assert_eq!(m.regs().sr, 0x2300);
}

#[test]
fn abi_layout_names() {
    let known = c"relevo_regs.pcr".as_ptr();
    assert_eq!(
        unsafe { relevo_abi_layout(known) },
        core::mem::offset_of!(relevo_regs, pcr) as u64
    );
    assert_eq!(unsafe { relevo_abi_layout(c"nothing".as_ptr()) }, u64::MAX);
}
