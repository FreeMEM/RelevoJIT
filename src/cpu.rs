// SPDX-License-Identifier: MPL-2.0
//! The state of a CPU, the run loop and what the API does with them.

use alloc::boxed::Box;
use alloc::sync::Arc;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering::Relaxed};

use crate::capi::{Handle, relevo_host};
use crate::interp::{self, Handler};
use crate::window::Window;

/// SR bits
pub const SR_C: u16 = 0x0001;
pub const SR_V: u16 = 0x0002;
pub const SR_Z: u16 = 0x0004;
pub const SR_N: u16 = 0x0008;
pub const SR_X: u16 = 0x0010;
pub const SR_IPL: u16 = 0x0700;
pub const SR_M: u16 = 0x1000;
pub const SR_S: u16 = 0x2000;
pub const SR_T0: u16 = 0x4000;
pub const SR_T1: u16 = 0x8000;

/// `RELEVO_OPT_EXIT_ON_PC_ZERO`
pub const OPT_EXIT_ON_PC_ZERO: u32 = 1;

/// Exception vector numbers
pub const VEC_BUS_ERROR: u32 = 2;
pub const VEC_ADDRESS_ERROR: u32 = 3;
pub const VEC_ILLEGAL: u32 = 4;
pub const VEC_ZERO_DIVIDE: u32 = 5;
pub const VEC_CHK: u32 = 6;
pub const VEC_TRAPV: u32 = 7;
pub const VEC_PRIVILEGE: u32 = 8;
pub const VEC_TRACE: u32 = 9;
pub const VEC_LINE_A: u32 = 10;
pub const VEC_LINE_F: u32 = 11;
pub const VEC_FORMAT_ERROR: u32 = 14;
pub const VEC_SPURIOUS: u32 = 24;
pub const VEC_AUTOVECTOR: u32 = 24; // + interrupt level 1-7
pub const VEC_TRAP: u32 = 32; // + 0-15

/// What a faulting access was doing: one of ACC_DATA and ACC_PROG (the low
/// bits of the function code) ORed with ACC_READ or ACC_WRITE. The values
/// are the bits of the 68000's special status word.
pub const ACC_DATA: u32 = 1;
pub const ACC_PROG: u32 = 2;
pub const ACC_WRITE: u32 = 0;
pub const ACC_READ: u32 = 0x10;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Model {
    M68000 = 0,
    M68010,
    M68020,
    M68030,
    M68040,
    M68060,
}

/// The FPU, as `relevo_fpu`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fpu {
    None = 0,
    M68881,
    M68882,
    /// The built-in FPU of the 68040 or 68060
    Internal,
}

/// Why `run()` returned, as `relevo_status`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    Budget = 0,
    ExitPcZero,
    Stopped,
    HostStop,
    Halted,
}

/// An FPU register, as `relevo_fpreg`.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct FpReg {
    pub mantissa: u64,
    pub sign_exponent: u16,
}

/// The registers, as `relevo_regs`: the host sees this structure.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Regs {
    pub d: [u32; 8],
    pub a: [u32; 8],
    pub pc: u32,
    pub sr: u16,
    pub usp: u32,
    pub isp: u32,
    pub msp: u32,
    pub vbr: u32,
    pub cacr: u32,
    pub caar: u32,
    pub sfc: u32,
    pub dfc: u32,
    pub fp: [FpReg; 8],
    pub fpcr: u32,
    pub fpsr: u32,
    pub fpiar: u32,
    pub tc: u32,
    pub itt0: u32,
    pub itt1: u32,
    pub dtt0: u32,
    pub dtt1: u32,
    pub mmusr: u32,
    pub urp: u32,
    pub srp: u32,
    pub buscr: u32,
    pub pcr: u32,
}

/// What other threads may touch while the CPU runs (`relevo_set_ipl()`,
/// `relevo_request_stop()`). It lives apart from [`Cpu`], which belongs to
/// the thread running it.
#[derive(Default)]
pub struct Shared {
    pub ipl: AtomicU32,
    pub stop_request: AtomicBool,
    /// For the JIT (RR3): set with either of the above.
    pub attention: AtomicU32,
}

/// A group 0 exception (address or bus error) or a double bus fault ended
/// the instruction: the exception has been taken (or the CPU halted), and
/// the run loop goes on with the next instruction. Every access that may
/// fault returns `Result<_, Fault>`, and handlers pass it up with `?`.
#[derive(Clone, Copy, Debug)]
pub struct Fault;

pub type Res<T = ()> = Result<T, Fault>;

/// The configuration, checked (from `relevo_config`).
pub struct Config {
    pub model: Model,
    pub fpu: Fpu,
    pub memory: *mut u8,
    pub memory_size: u64,
    pub address_mask: u32,
    pub options: u32,
    pub host: relevo_host,
}

pub struct Cpu {
    pub r: Regs,

    pub mem: Window,
    pub addr_mask: u32,
    /// SR bits the model implements
    pub sr_mask: u16,
    pub model: Model,
    pub fpu: Fpu,
    pub options: u32,
    pub host: relevo_host,
    /// What the host knows this CPU as (`relevo_cpu *`), for its functions.
    pub handle: *mut Handle,
    pub shared: Arc<Shared>,

    /// Address of the instruction being run
    pub insn_pc: u32,
    /// Set by a handler to end `run()`
    pub exit: Status,
    pub exiting: bool,
    /// After STOP, until an interrupt
    pub stopped: bool,
    /// Double bus fault, until a reset
    pub halted: bool,
    /// Word and long data at odd addresses fault (68000, 68010)
    pub align_check: bool,
    /// Instructions executed
    pub count: u64,
    /// Clock cycles, 68000 timing: 4 per bus access of a byte or word (8 for
    /// a long), counted by the accesses (memory.rs), plus what each handler
    /// adds for internal operations. Every word of an instruction is counted
    /// once when fetched: this stands for the 68000 refilling its prefetch
    /// queue.
    pub cycles: u64,
    /// Cycles a group 0 exception adds to or takes from the instruction's
    /// count, where the 68000's prefetches are ahead of or behind the words
    /// counted so far (MOVE).
    pub fault_cycles: i32,
    /// Left in the current `run()`
    pub budget: u64,

    /// Trace: set at the start of an instruction from SR.T1, taken before
    /// the next one unless the instruction cleared it (group 0 and group 1
    /// exceptions do).
    pub trace_pending: bool,

    /// Between fetch and the end of a handler
    pub in_insn: bool,
    /// A fault now is a double bus fault
    pub in_group0: bool,
    /// What the 68000 stacks in a group 0 frame besides the address: the
    /// instruction register (normally the opcode) and the PC at the fault,
    /// which depends on how far the instruction got (see interp/ea.rs).
    pub ir: u16,
    pub ae_pc: u32,
    /// Bytes of the access that took a bus error (for the 68010+ frames)
    pub fault_size: u32,

    /// FPU (interp/fpu.rs): the vector of an enabled exception to take
    /// before the next floating-point instruction (0: none), and whether
    /// FSAVE gives a null frame (after a reset, until an FPU instruction).
    pub fpu_pending: u32,
    pub fpu_null: bool,

    /// Level 7 is edge triggered
    pub prev_ipl: u32,

    pub table: Box<[Handler; 0x10000]>,
}

impl Cpu {
    /// A CPU on the window `mem` (already holding the host's memory block).
    pub fn new(cfg: &Config, mem: Window, shared: Arc<Shared>) -> Cpu {
        let mut addr_mask = cfg.address_mask;
        if addr_mask == 0 {
            addr_mask = if cfg.model <= Model::M68010 {
                0x00FF_FFFF
            } else {
                0xFFFF_FFFF
            };
        }
        let sr_mask = match cfg.model {
            Model::M68020 | Model::M68030 | Model::M68040 => 0xF71F, // T1 T0 S M
            _ => 0xA71F,                                             // T1 S
        };
        let (table, _) = interp::build_table(cfg.model, cfg.fpu);
        let mut c = Cpu {
            r: Regs::default(),
            mem,
            addr_mask,
            sr_mask,
            model: cfg.model,
            fpu: cfg.fpu,
            options: cfg.options,
            host: cfg.host,
            handle: core::ptr::null_mut(),
            shared,
            insn_pc: 0,
            exit: Status::Budget,
            exiting: false,
            stopped: false,
            halted: false,
            align_check: cfg.model <= Model::M68010,
            count: 0,
            cycles: 0,
            fault_cycles: 0,
            budget: 0,
            trace_pending: false,
            in_insn: false,
            in_group0: false,
            ir: 0,
            ae_pc: 0,
            fault_size: 0,
            fpu_pending: 0,
            fpu_null: true,
            prev_ipl: 0,
            table,
        };
        if cfg.memory_size != 0 {
            c.mem.map(0, cfg.memory_size, crate::window::PAGE_RAM);
        }
        c.r.sr = SR_S | SR_IPL;
        c.fpu_reset();
        c
    }

    /// Sets SR as a MOVE to SR does: masks the bits the model lacks and
    /// switches a[7] between usp, isp and msp.
    pub fn set_sr(&mut self, sr: u16) {
        let sr = sr & self.sr_mask;
        let a7 = self.r.a[7];
        *self.sp_slot(self.r.sr) = a7;
        self.r.sr = sr;
        self.r.a[7] = *self.sp_slot(sr);
    }

    /// Where the stack pointer for a given SR is kept while it is not
    /// active.
    fn sp_slot(&mut self, sr: u16) -> &mut u32 {
        if sr & SR_S == 0 {
            &mut self.r.usp
        } else if sr & SR_M != 0 {
            &mut self.r.msp
        } else {
            &mut self.r.isp
        }
    }

    /// Reset: supervisor mode, interrupts masked, VBR 0, ISP and PC read
    /// from addresses 0 and 4.
    pub fn reset(&mut self) {
        self.r.sr = SR_S | SR_IPL;
        self.r.vbr = 0;
        self.r.cacr = 0;
        self.stopped = false;
        self.halted = false;
        self.trace_pending = false;
        self.in_group0 = false;
        self.fpu_reset();
        self.r.isp = self.mem_read32(0);
        self.r.a[7] = self.r.isp;
        self.r.pc = self.mem_read32(4);
    }

    /// Runs one instruction in the interpreter (the checks between
    /// instructions are the caller's), counting it against the budget.
    #[inline(always)]
    fn step(&mut self) -> Res {
        let pc = self.r.pc;
        self.insn_pc = pc;
        self.trace_pending = self.r.sr & SR_T1 != 0;
        self.ae_pc = pc.wrapping_add(2);
        self.fault_cycles = 0;
        self.in_insn = true;
        if pc & 1 != 0 {
            return Err(self.address_error(pc, ACC_PROG | ACC_READ));
        }
        let op = self.read16(pc, ACC_PROG)? as u16;
        self.ir = op;
        self.r.pc = pc.wrapping_add(2);
        let handler = self.table[usize::from(op)];
        handler(self, op)?;
        self.in_insn = false;
        self.count += 1;
        self.budget -= 1;
        Ok(())
    }

    /// Whether `between_insns()` has anything to do.
    #[inline(always)]
    fn between_needed(&self) -> bool {
        let ipl = self.shared.ipl.load(Relaxed);
        self.trace_pending
            || self.stopped
            || ipl != self.prev_ipl
            || ipl > u32::from((self.r.sr & SR_IPL) >> 8)
    }

    fn run_loop(&mut self) -> Res<Status> {
        while self.budget != 0 {
            if self.shared.stop_request.load(Relaxed) {
                self.shared.stop_request.store(false, Relaxed);
                return Ok(Status::HostStop);
            }
            if self.between_needed() && !self.between_insns()? {
                return Ok(Status::Stopped);
            }
            self.step()?;
            if self.exiting {
                return Ok(self.exit);
            }
            if self.r.pc == 0 && self.options & OPT_EXIT_ON_PC_ZERO != 0 {
                return Ok(Status::ExitPcZero);
            }
        }
        Ok(Status::Budget)
    }

    /// Runs until `budget` instructions have executed or another reason to
    /// stop comes up.
    pub fn run(&mut self, budget: u64) -> Status {
        if self.halted {
            return Status::Halted;
        }
        self.budget = budget;
        self.exiting = false;
        loop {
            match self.run_loop() {
                Ok(st) => return st,
                Err(Fault) => {
                    // A group 0 exception ended the instruction.
                    if self.in_insn {
                        self.in_insn = false;
                        self.count += 1;
                        self.budget -= 1;
                    }
                    if self.halted {
                        return Status::Halted;
                    }
                }
            }
        }
    }
}
