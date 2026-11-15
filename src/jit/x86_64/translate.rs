// SPDX-License-Identifier: MPL-2.0
//! The x86-64 code generator.
//!
//! Inside a block:
//!
//! ```text
//!   RBX  the Cpu (registers, counters, the JIT's tables)
//!   R15  the base of the 68k memory: address a is at [R15 + a]
//!   R14  SR (the CCR bits change, the rest is only stored back)
//!   RSI RDI RBP R8-R13  68k registers, given out per block in order of use
//!   RAX RCX RDX  scratch
//! ```
//!
//! A block starts by checking the budget (enough for all its instructions)
//! and the attention flag (an interrupt level change, a stop request); if
//! either fails it leaves at once. Then it loads every 68k register it
//! uses; every one it changes is stored at each exit.
//!
//! Each instruction is emitted in two phases: first its addresses and their
//! checks, which may leave the block through a "bail" exit with nothing of
//! the instruction done; then its effects. The check of an access looks up
//! the JIT's map of 4 KB pages and the page offset; what the map does not
//! clear at once (a page with translated code, an access across pages)
//! calls the access check through a shared stub. Alignment on the 68000
//! and 68010, host pages, missing pages and writes into translated code
//! all bail (a host page or a ROM write goes to the host path instead:
//! the interpreter runs that one instruction from the block).
//!
//! Flags: the x86 flags of an operation are N, Z, V and C (SF, ZF, OF and
//! CF; for subtraction x86's CF is the borrow, as on the 68k), and X is C
//! where the 68k sets it. They are kept only when something reads them
//! later (an instruction of the block, the block's exit, or an instruction
//! that may bail), and not at once: they stay "pending" in EFLAGS until
//! some code would change them. So a Bcc, DBcc or Scc right after the
//! instruction that set its flags tests EFLAGS directly.
//!
//! Lazy condition codes (when the CPU has LAHF and SAHF in 64-bit mode):
//! pending flags that must be kept are not turned into CCR bits but saved
//! as they are, with LAHF and SETO, in `Cpu::jit_ccr` (the "record"),
//! together with which CCR bits they give; X has its own byte. A CCR bit
//! then lives in EFLAGS (pending), in R14 (when the block knows R14 has it:
//! "own"), or else in the record if the record has it, or else in R14.
//! Saving takes four or five instructions and changes no flag, and SAHF
//! brings the flags back for a Bcc. The record goes from block to block: a
//! block that does not touch the flags leaves it as it is, and one that
//! ends with pending flags overwrites it. Only what reads CCR bits from R14
//! (ADDX, the handler calls, a condition with mixed sources...) merges it
//! first, through a shared stub, and the dispatcher merges it into SR when
//! a block returns. SR itself stays in R14 from block to block: the
//! trampoline loads it and the common exit stores it.
//!
//! Exits: a branch to a known PC ends in a jump that first leads to the
//! dispatcher (EXIT_CHAIN) and is then patched to go straight to the block
//! there. RTS and jumps through registers look the target up in the cache
//! of indirect targets. A branch to the block's own start loops inside it
//! while the budget lasts and nothing asks for attention. Everything leaves
//! through the common exit with EAX the PC, ECX the exit code and EDX the
//! chained jump's offset.
//!
//! Instructions too rare or too intricate to translate, but with only
//! register operands and no exception possible (shifts by a register,
//! ABCD, bit fields in Dn...), call their interpreter handler from the
//! block (Kind::Call).
//!
//! Every call out of translated code uses the System V convention (the
//! callbacks are `extern "sysv64"`), on Windows too.

use alloc::vec::Vec;
use core::mem::offset_of;

use super::emit::*;
use crate::cpu::{Cpu, Model, Regs};
use crate::interp::{SZ_W, sext, sz_mask, sz_msb};
use crate::jit::decode::{
    A_ADD, A_AND, A_CMP, A_EOR, A_OR, A_SUB, B_TST, E_L, E_W, Ea, EaKind, F_ALL, F_C, F_N, F_NZVC,
    F_V, F_X, F_Z, Insn, Kind, S_AS, S_LS, S_RO, U_NEG, U_NOT, cond_table, ra,
};
use crate::jit::{
    EXIT_BAIL, EXIT_CHAIN, EXIT_NORMAL, IND_SIZE, JMAP_CODE, JMAP_NO_READ, JMAP_NO_WRITE,
    MAP_SHIFT, MAX_INSNS,
};

const RCPU: R = RBX;
const RMEM: R = R15;
const RSR: R = R14;

/// x86 registers a block can give to 68k registers.
pub const HOST_REGS: usize = 9;
const HOST_POOL: [R; HOST_REGS] = [RSI, RDI, RBP, R8, R9, R10, R11, R12, R13];

const fn off_reg(n: usize) -> i32 {
    (offset_of!(Cpu, r)
        + if n < 8 {
            offset_of!(Regs, d) + 4 * n
        } else {
            offset_of!(Regs, a) + 4 * (n - 8)
        }) as i32
}
const OFF_PC: i32 = (offset_of!(Cpu, r) + offset_of!(Regs, pc)) as i32;
const OFF_SR: i32 = (offset_of!(Cpu, r) + offset_of!(Regs, sr)) as i32;
const OFF_FPCR: i32 = (offset_of!(Cpu, r) + offset_of!(Regs, fpcr)) as i32;
const OFF_BUDGET: i32 = offset_of!(Cpu, budget) as i32;
const OFF_CYCLES: i32 = offset_of!(Cpu, cycles) as i32;
const OFF_ATTN: i32 = offset_of!(Cpu, jit_attn) as i32;
const OFF_IR: i32 = offset_of!(Cpu, ir) as i32;
const OFF_MAP: i32 = offset_of!(Cpu, jit_map) as i32;
const OFF_IND: i32 = offset_of!(Cpu, jit_ind) as i32;
const OFF_CCR: i32 = offset_of!(Cpu, jit_ccr) as i32;
const OFF_FPEND: i32 = offset_of!(Cpu, fpu_pending) as i32;

// The record is read and written by bytes; AH is stored with no REX.
const _: () = assert!(OFF_CCR % 4 == 0);

/// Pending flags: what EFLAGS holds for the CCR.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Pm {
    None,
    Logic,
    Arith,
    ArithX,
}

#[derive(Clone, Copy)]
struct Pending {
    mask: u8,
    mode: Pm,
}

const NO_PEND: Pending = Pending {
    mask: 0,
    mode: Pm::None,
};

#[derive(Clone, Copy)]
struct Deferred {
    slow: i32,
    cont: i32,
    bail: i32,
    host: i32,
    /// the register with the masked address
    addr: R,
    /// size | write << 16
    size_write: u32,
}

/// The flag state, for code emitted on two paths.
#[derive(Clone, Copy)]
struct FState {
    pend: Pending,
    own: u8,
    rec: u8,
}

/// A value: an x86 register or a constant.
#[derive(Clone, Copy)]
struct Val {
    /// -1: constant
    reg: R,
    imm: u32,
}

fn vreg(r: R) -> Val {
    Val { reg: r, imm: 0 }
}

fn vimm(v: u32) -> Val {
    Val { reg: -1, imm: v }
}

/// What the generated code calls, and where the shared stubs are.
#[derive(Clone, Copy)]
pub struct Env {
    pub model: Model,
    pub addr_mask: u32,
    pub align_check: bool,
    pub movbe: bool,
    pub lazy: bool,
    pub exit_off: usize,
    pub check_off: usize,
    pub ccr_off: usize,
    pub host_step: u64,
    pub call_handler: u64,
    pub fpu_op: u64,
    pub fpu_branch: u64,
}

/// The translator's working state, kept between blocks.
pub struct Tr {
    b: XBuf,
    /// where the code goes in the code memory
    base: usize,
    env: Env,
    ins: Vec<Insn>,
    n: usize,
    /// 68k register -> x86 register, or -1
    host: [R; 16],
    used: u16,
    written: u16,
    /// (An)+/-(An) not applied yet
    delta: [i32; 16],
    live_out: [u8; MAX_INSNS],
    cyc_before: [u32; MAX_INSNS + 1],
    /// label of each instruction's bail exit, or -1
    bail: [i32; MAX_INSNS],
    /// ... and of its host path, or -1
    hostp: [i32; MAX_INSNS],
    /// memory checks of each instruction
    naccess: [u8; MAX_INSNS],
    tail: i32,
    loop_: i32,
    entry_out: i32,
    pend: Pending,
    /// CCR bits R14 holds for sure
    own: u8,
    /// CCR bits the record holds for sure
    rec: u8,
    /// the instruction being emitted
    cur: usize,
    /// `own` at each bail check
    own_at: [u8; MAX_INSNS],
    label_pos: Vec<i64>,
    fix: Vec<(usize, i32)>,
    defer: Vec<Deferred>,
    failed: bool,
}

const MAX_LABELS: usize = 16 * MAX_INSNS + 16;

impl Tr {
    pub fn new(env: Env) -> Tr {
        Tr {
            b: XBuf::default(),
            base: 0,
            env,
            ins: Vec::with_capacity(MAX_INSNS),
            n: 0,
            host: [-1; 16],
            used: 0,
            written: 0,
            delta: [0; 16],
            live_out: [0; MAX_INSNS],
            cyc_before: [0; MAX_INSNS + 1],
            bail: [-1; MAX_INSNS],
            hostp: [-1; MAX_INSNS],
            naccess: [0; MAX_INSNS],
            tail: 0,
            loop_: 0,
            entry_out: 0,
            pend: NO_PEND,
            own: 0,
            rec: 0,
            cur: 0,
            own_at: [0; MAX_INSNS],
            label_pos: Vec::with_capacity(MAX_LABELS),
            fix: Vec::new(),
            defer: Vec::new(),
            failed: false,
        }
    }

    pub fn set_env(&mut self, env: Env) {
        self.env = env;
    }

    // ---- Labels ----

    fn new_label(&mut self) -> i32 {
        self.label_pos.push(-1);
        (self.label_pos.len() - 1) as i32
    }

    fn bind(&mut self, l: i32) {
        self.label_pos[l as usize] = self.b.pos() as i64;
    }

    fn add_fix(&mut self, at: usize, l: i32) {
        if l < 0 {
            self.failed = true;
            return;
        }
        self.fix.push((at, l));
    }

    fn jcc(&mut self, cc: u8, l: i32) {
        let at = self.b.jcc32(cc);
        self.add_fix(at, l);
    }

    fn jmp(&mut self, l: i32) {
        let at = self.b.jmp32();
        self.add_fix(at, l);
    }

    /// A jump or call to shared code at offset `off` of the code memory.
    fn link_abs(&mut self, at: usize, off: usize) {
        let rel = off as i64 - (self.base + at + 4) as i64;
        self.b.patch32(at, rel as u32);
    }

    fn jmp_abs(&mut self, off: usize) {
        let at = self.b.jmp32();
        self.link_abs(at, off);
    }

    fn call_abs(&mut self, off: usize) {
        let at = self.b.call32();
        self.link_abs(at, off);
    }

    // ---- Registers ----

    fn h(&self, r: usize) -> R {
        self.host[r]
    }

    fn hr(&self, r: u8) -> R {
        self.host[usize::from(r)]
    }

    /// The x86 register with 68k register r: its own, or `tmp` loaded from
    /// memory if the block does not hold it.
    fn reg_get(&mut self, r: usize, tmp: R) -> R {
        if self.h(r) >= 0 {
            return self.h(r);
        }
        self.b.load(4, tmp, xm(RCPU, off_reg(r)));
        tmp
    }

    fn apply_deltas(&mut self) {
        for r in 0..16 {
            if self.delta[r] != 0 {
                let h = self.h(r);
                self.b.lea(h, xm(h, self.delta[r]));
                self.delta[r] = 0;
            }
        }
    }

    // ---- Flags ----

    fn pushf_rax(&mut self) {
        self.b.pushfq();
        self.b.pop(RAX);
    }

    /// ORs into R14 the bits of `mask` among C, X, V of the flags in EAX
    /// (pushed EFLAGS). Uses RCX.
    fn gather_cvx(&mut self, mask: u8, with_x: bool) {
        let b = &mut self.b;
        if mask & (F_C | F_X) != 0 {
            b.mov_rr(4, RCX, RAX);
            b.alu_ri(XA_AND, 4, RCX, 1);
            if mask & F_C != 0 {
                b.alu_rr(XA_OR, 4, RSR, RCX);
            }
            if with_x && mask & F_X != 0 {
                b.shift_ri(4, RCX, 4);
                b.alu_rr(XA_OR, 4, RSR, RCX);
            }
        }
        if mask & F_V != 0 {
            b.mov_rr(4, RCX, RAX);
            b.shift_ri(5, RCX, 10);
            b.alu_ri(XA_AND, 4, RCX, u32::from(F_V));
            b.alu_rr(XA_OR, 4, RSR, RCX);
        }
    }

    /// ORs into R14 N and Z (of `mask`) from the flags in EAX. Uses RCX.
    fn gather_nz(&mut self, mask: u8) {
        let b = &mut self.b;
        if mask & (F_N | F_Z) != 0 {
            b.mov_rr(4, RCX, RAX);
            b.shift_ri(5, RCX, 4);
            b.alu_ri(XA_AND, 4, RCX, u32::from(mask & (F_N | F_Z)));
            b.alu_rr(XA_OR, 4, RSR, RCX);
        }
    }

    /// The CCR bits in `mask` from EFLAGS, as an operation of `mode` left
    /// them. Uses RAX and RCX.
    fn flags_from_eflags(&mut self, mask: u8, mode: Pm) {
        if mask == 0 {
            return;
        }
        let nmask = !u32::from(mask);
        if mode == Pm::Logic {
            let b = &mut self.b;
            let nz = mask & (F_N | F_Z);
            if nz == F_Z || nz == F_N {
                b.setcc(if nz == F_Z { XC_E } else { XC_S }, RAX);
                b.movzx_rr(1, RAX, RAX);
                b.shift_ri(4, RAX, if nz == F_Z { 2 } else { 3 });
            } else if nz != 0 {
                self.pushf_rax();
                let b = &mut self.b;
                b.shift_ri(5, RAX, 4);
                b.alu_ri(XA_AND, 4, RAX, u32::from(F_N | F_Z));
            }
            let b = &mut self.b;
            b.alu_ri(XA_AND, 4, RSR, nmask);
            if nz != 0 {
                b.alu_rr(XA_OR, 4, RSR, RAX);
            }
            return;
        }
        self.pushf_rax();
        if mask & F_NZVC == F_NZVC {
            // ECX = N Z V C, built from EAX; then X = C if needed
            let b = &mut self.b;
            b.mov_rr(4, RCX, RAX);
            b.shift_ri(5, RCX, 4);
            b.alu_ri(XA_AND, 4, RCX, u32::from(F_N | F_Z));
            b.btx_ri(XB_BT, RAX, 0);
            b.alu_ri(XA_ADC, 4, RCX, 0);
            b.shift_ri(5, RAX, 10);
            b.alu_ri(XA_AND, 4, RAX, u32::from(F_V));
            b.alu_rr(XA_OR, 4, RCX, RAX);
            b.alu_ri(XA_AND, 4, RSR, nmask);
            b.alu_rr(XA_OR, 4, RSR, RCX);
            if mode == Pm::ArithX && mask & F_X != 0 {
                b.alu_ri(XA_AND, 4, RCX, u32::from(F_C));
                b.shift_ri(4, RCX, 4);
                b.alu_rr(XA_OR, 4, RSR, RCX);
            }
            return;
        }
        self.b.alu_ri(XA_AND, 4, RSR, nmask);
        self.gather_nz(mask);
        self.gather_cvx(mask, mode == Pm::ArithX);
    }

    /// Saves the pending flags in the record, as they are. Changes no
    /// flag: EFLAGS still has them.
    fn record_pending(&mut self) {
        let m = self.pend.mask;
        self.b.lahf();
        if m & F_NZVC != 0 {
            self.b.setcc(XC_O, RAX);
            self.b.store(2, xm(RCPU, OFF_CCR), RAX);
            self.b
                .store_imm(1, xm(RCPU, OFF_CCR + 2), u32::from(m & F_NZVC));
            self.rec = (self.rec & F_X) | (m & F_NZVC);
        }
        if m & F_X != 0 {
            self.b.store_ah(xm(RCPU, OFF_CCR + 3));
            self.rec |= F_X;
        }
        self.own &= !m;
        self.pend = NO_PEND;
    }

    /// Whether flags pending now are read from R14 later in the block (by
    /// an instruction other than a condition, which can take them back from
    /// the record): then they are better turned into CCR bits at once.
    fn read_from_r14_later(&self) -> bool {
        let mut left = self.pend.mask;
        for i in &self.ins[self.cur..self.n] {
            if left == 0 {
                break;
            }
            let cond = matches!(i.kind, Kind::Bcc | Kind::Dbcc | Kind::Scc);
            if i.flags_use & left != 0 && !cond {
                return true;
            }
            left &= !i.flags_def;
        }
        false
    }

    /// Keeps the pending flags before something changes EFLAGS: in the
    /// record or in R14.
    fn flags_flush(&mut self) {
        if self.pend.mode == Pm::None {
            return;
        }
        if self.env.lazy && !self.read_from_r14_later() {
            self.record_pending();
            return;
        }
        self.flags_from_eflags(self.pend.mask, self.pend.mode);
        self.own |= self.pend.mask;
        self.rec &= !self.pend.mask;
        self.pend = NO_PEND;
    }

    fn flags_pending(&mut self, mask: u8, mode: Pm) {
        self.pend = Pending {
            mask,
            mode: if mask != 0 { mode } else { Pm::None },
        };
    }

    /// Makes R14 hold the CCR bits in `need` (nothing pending): if the
    /// record may have one of them, the stub merges it into R14 (keeping
    /// what the block wrote there) and empties it. Uses RAX, RCX and RDX.
    fn flags_need(&mut self, need: u8) {
        let miss = need & !self.own;
        if miss == 0 {
            return;
        }
        self.b.test_mi32(
            xm(RCPU, OFF_CCR),
            u32::from(miss & F_NZVC) << 16 | if miss & F_X != 0 { 0xFF00_0000 } else { 0 },
        );
        let skip = self.new_label();
        self.jcc(XC_E, skip);
        self.b.mov_ri(RCX, u32::from(self.own));
        self.call_abs(self.env.ccr_off);
        self.bind(skip);
        self.own |= need;
        self.rec = 0;
    }

    /// Where a block leaves (or bails): makes R14 and the record agree. The
    /// pending flags go to the record; what R14 holds for sure is taken out
    /// of the record. Changes no flag while flags are pending.
    fn flags_exit(&mut self) {
        if !self.env.lazy {
            self.flags_flush();
            return;
        }
        if self.pend.mode != Pm::None && F_NZVC & !self.pend.mask & !self.own != 0 {
            // (never at an exit: pending flags have all of N Z V C)
            self.flags_from_eflags(self.pend.mask, self.pend.mode);
            self.own |= self.pend.mask;
            self.pend = NO_PEND;
        }
        if self.pend.mode != Pm::None {
            let m = self.pend.mask;
            let x_own = self.own & F_X != 0 && m & F_X == 0;
            self.record_pending();
            if x_own {
                self.b.store_imm(1, xm(RCPU, OFF_CCR + 3), 0);
            }
            return;
        }
        let nzvc = self.own & F_NZVC;
        let x = self.own & F_X != 0;
        let b = &mut self.b;
        if nzvc == F_NZVC && x {
            b.store_imm(2, xm(RCPU, OFF_CCR + 2), 0);
            return;
        }
        if nzvc == F_NZVC {
            b.store_imm(1, xm(RCPU, OFF_CCR + 2), 0);
        } else if nzvc != 0 {
            b.alu_mi(XA_AND, 1, xm(RCPU, OFF_CCR + 2), i32::from(!nzvc));
        }
        if x {
            b.store_imm(1, xm(RCPU, OFF_CCR + 3), 0);
        }
    }

    /// Before a branch: pending flags go to the record now (it changes no
    /// flag), so that every way out finds them there.
    fn flags_settle(&mut self) {
        if self.env.lazy && self.pend.mode != Pm::None {
            self.record_pending();
        }
    }

    fn fs_save(&self) -> FState {
        FState {
            pend: self.pend,
            own: self.own,
            rec: self.rec,
        }
    }

    fn fs_restore(&mut self, f: FState) {
        self.pend = f.pend;
        self.own = f.own;
        self.rec = f.rec;
    }

    /// The CCR bits in `mask` from constants.
    fn flags_const(&mut self, mask: u8, bits: u8) {
        if mask == 0 {
            return;
        }
        self.b.alu_ri(XA_AND, 4, RSR, !u32::from(mask));
        if bits & mask != 0 {
            self.b.alu_ri(XA_OR, 4, RSR, u32::from(bits & mask));
        }
    }

    /// The CCR bits in `mask`: N and Z from SF and ZF, C from CF, the low
    /// bit of DL or 0, V from OF, DL or 0 (only one of them from DL), and X
    /// = C if `x_eq_c`. Uses RAX, RCX and RDX.
    fn flags_build(&mut self, mask: u8, csrc: Fb, vsrc: Fb, x_eq_c: bool) {
        if mask == 0 {
            return;
        }
        if csrc == Fb::Dl || vsrc == Fb::Dl {
            self.b.movzx_rr(1, RDX, RDX);
        }
        self.pushf_rax();
        self.b.alu_ri(XA_AND, 4, RSR, !u32::from(mask));
        self.gather_nz(mask);
        match csrc {
            Fb::Cf => self.gather_cvx(mask & (F_C | if x_eq_c { F_X } else { 0 }), true),
            Fb::Dl => {
                if mask & F_C != 0 {
                    self.b.alu_rr(XA_OR, 4, RSR, RDX);
                }
                if x_eq_c && mask & F_X != 0 {
                    self.b.mov_rr(4, RCX, RDX);
                    self.b.shift_ri(4, RCX, 4);
                    self.b.alu_rr(XA_OR, 4, RSR, RCX);
                }
            }
            Fb::Zero => {}
        }
        match vsrc {
            Fb::Cf => self.gather_cvx(mask & F_V, false),
            Fb::Zero => {}
            Fb::Dl => {
                if mask & F_V != 0 {
                    self.b.lea(RCX, xmi(RDX, RDX, 0, 0));
                    self.b.alu_rr(XA_OR, 4, RSR, RCX);
                }
            }
        }
    }

    /// ADDX, SUBX, NEGX: all from EFLAGS with X = C, but Z only cleared.
    fn flags_sticky(&mut self, mask: u8) {
        if mask == 0 {
            return;
        }
        self.pushf_rax();
        if mask & F_Z != 0 {
            // R14 &= ZF ? ~0 : ~Z
            let b = &mut self.b;
            b.mov_rr(4, RCX, RAX);
            b.shift_ri(5, RCX, 4);
            b.alu_ri(XA_AND, 4, RCX, u32::from(F_Z));
            b.alu_ri(XA_OR, 4, RCX, !u32::from(F_Z));
            b.alu_rr(XA_AND, 4, RSR, RCX);
        }
        let m = mask & !F_Z;
        self.b.alu_ri(XA_AND, 4, RSR, !u32::from(m));
        self.gather_nz(m);
        self.gather_cvx(m, true);
    }

    /// CF = condition `cc` holds for the CCR in R14. Uses RAX and RCX.
    fn cond_to_cf(&mut self, cc: u32) {
        let table = cond_table(cc);
        let b = &mut self.b;
        b.mov_rr(4, RAX, RSR);
        b.alu_ri(XA_AND, 4, RAX, 15);
        b.mov_ri(RCX, table);
        b.bt_rr(RCX, RAX);
    }

    /// The x86 condition for 68k condition cc (2-15), which reads the CCR
    /// bits `need`: on EFLAGS while flags are pending (they hold N, Z, V
    /// and C exactly), on EFLAGS taken back from the record when it has
    /// them all (SAHF; ADD AL first sets OF from the byte SETO wrote), else
    /// on CF after testing R14 (emit_insn made sure R14 has them). Uses RAX
    /// and RCX.
    fn cond(&mut self, cc: u32, need: u8) -> u8 {
        const X86CC: [u8; 16] = [
            0, 0, XC_A, XC_BE, XC_AE, XC_B, XC_NE, XC_E, XC_NO, XC_O, XC_NS, XC_S, XC_GE, XC_L,
            XC_G, XC_LE,
        ];
        if self.pend.mode != Pm::None {
            return X86CC[(cc & 15) as usize];
        }
        if need & !self.own != 0 {
            self.b.load(2, RAX, xm(RCPU, OFF_CCR));
            self.b.alu_ri(XA_ADD, 1, RAX, 0x7F);
            self.b.sahf();
            return X86CC[(cc & 15) as usize];
        }
        self.cond_to_cf(cc);
        XC_B
    }

    // ---- Exits ----

    /// Adds what instructions [0, k) did to the counters. (The instruction
    /// count is the dispatcher's: what the budget went down by.)
    fn count(&mut self, k: usize) {
        if k == 0 {
            return;
        }
        self.b.alu_mi(XA_SUB, 8, xm(RCPU, OFF_BUDGET), k as i32);
        self.b
            .alu_mi(XA_ADD, 8, xm(RCPU, OFF_CYCLES), self.cyc_before[k] as i32);
    }

    fn store_regs(&mut self) {
        for r in 0..16 {
            if self.written & (1 << r) != 0 {
                let h = self.h(r);
                self.b.store(4, xm(RCPU, off_reg(r)), h);
            }
        }
    }

    /// What every exit after the instructions stores: the registers and,
    /// for the 68000's fault frames, IR (the last opcode run). (SR stays in
    /// R14: the common exit stores it.)
    fn exit_stores(&mut self) {
        self.store_regs();
        if self.env.model == Model::M68000 {
            let op = self.ins[self.n - 1].op;
            self.b.store_imm(2, xm(RCPU, OFF_IR), u32::from(op));
        }
    }

    /// Leaves the block for a known PC after all its instructions: through
    /// a jump that the dispatcher may chain to the block at `pc`.
    fn exit_chain(&mut self, pc: u32) {
        self.flags_exit();
        self.count(self.n);
        self.exit_stores();
        let at = self.b.jmp32(); // rel 0: falls through, until patched
        self.b.mov_ri(RAX, pc);
        self.b.mov_ri(RCX, EXIT_CHAIN);
        self.b.mov_ri(RDX, (self.base + at) as u32);
        self.jmp_abs(self.env.exit_off);
    }

    /// Leaves for the PC in `r` (not RAX or RCX): through the cache of
    /// indirect targets, else to the dispatcher.
    fn exit_indirect(&mut self, r: R) {
        self.flags_exit();
        self.count(self.n);
        self.exit_stores();
        let b = &mut self.b;
        b.mov_rr(4, RAX, r);
        b.mov_rr(4, RCX, RAX);
        b.alu_ri(XA_AND, 4, RCX, ((IND_SIZE - 1) << 1) as u32);
        b.shift_ri(4, RCX, 3);
        b.add_rm64(RCX, xm(RCPU, OFF_IND));
        b.cmp_mr(xm(RCX, 0), RAX);
        let miss = self.new_label();
        self.jcc(XC_NE, miss);
        self.b.jmp_m(xm(RCX, 8));
        self.bind(miss);
        self.b.mov_ri(RCX, EXIT_NORMAL);
        self.jmp_abs(self.env.exit_off);
    }

    /// A branch taken to `target`: back to the top of the block if that is
    /// where it starts, while the budget lasts and nothing needs attention.
    fn exit_branch(&mut self, target: u32) {
        if target != self.ins[0].pc {
            self.exit_chain(target);
            return;
        }
        let out = self.new_label();
        self.flags_exit();
        self.count(self.n);
        self.b
            .alu_mi(XA_CMP, 8, xm(RCPU, OFF_BUDGET), self.n as i32);
        self.jcc(XC_B, out);
        self.b.load64(RAX, xm(RCPU, OFF_ATTN));
        self.b.alu_mi(XA_CMP, 4, xm(RAX, 0), 0);
        self.jcc(XC_NE, out);
        self.jmp(self.loop_);
        self.bind(out);
        self.b.mov_ri(RAX, target);
        self.b.mov_ri(RCX, EXIT_NORMAL);
        self.jmp(self.tail);
    }

    // ---- Memory ----

    /// Reads `size` bytes (big-endian) at [R15 + addr + disp] into `dst`,
    /// zero-extended.
    fn load_be(&mut self, size: u32, dst: R, addr: R, disp: i32) {
        let m = xmi(RMEM, addr, 0, disp);
        let movbe = self.env.movbe;
        let b = &mut self.b;
        match size {
            1 => b.movzx_load(1, dst, m),
            2 => {
                if movbe {
                    b.movbe_load(2, dst, m);
                    b.movzx_rr(2, dst, dst);
                } else {
                    b.movzx_load(2, dst, m);
                    b.rol16_8(dst);
                }
            }
            _ => {
                if movbe {
                    b.movbe_load(4, dst, m);
                } else {
                    b.load(4, dst, m);
                    b.bswap32(dst);
                }
            }
        }
    }

    /// Writes a value big-endian at [R15 + addr + disp], without changing
    /// the x86 flags. May use RCX.
    fn store_be(&mut self, size: u32, addr: R, disp: i32, v: Val) {
        let m = xmi(RMEM, addr, 0, disp);
        let movbe = self.env.movbe;
        let b = &mut self.b;
        let sz = size as u8;
        if v.reg < 0 {
            let swapped = match size {
                1 => v.imm & 0xFF,
                2 => u32::from((v.imm as u16).swap_bytes()),
                _ => v.imm.swap_bytes(),
            };
            b.store_imm(sz, m, swapped);
            return;
        }
        if size == 1 {
            b.store(1, m, v.reg);
        } else if movbe {
            b.movbe_store(sz, m, v.reg);
        } else {
            if v.reg != RCX {
                b.mov_rr(4, RCX, v.reg);
            }
            if size == 2 {
                b.xchg_cl_ch();
            } else {
                b.bswap32(RCX);
            }
            b.store(sz, m, RCX);
        }
    }

    /// The host path of the instruction being emitted (see host_path()).
    fn host_label(&mut self) -> i32 {
        if self.hostp[self.cur] < 0 {
            self.hostp[self.cur] = self.new_label();
        }
        self.hostp[self.cur]
    }

    fn defer(&mut self, slow: i32, cont: i32, bail: i32, addr: R, size: u32, write: bool) {
        let host = self.host_label();
        self.defer.push(Deferred {
            slow,
            cont,
            bail,
            host,
            addr,
            size_write: size | if write { 1 << 16 } else { 0 },
        });
    }

    /// Checks that `size` bytes at the address in `addr` are a plain access
    /// (else bails), and masks the address. `align`: a word or long access,
    /// which faults at an odd address on the 68000 and 68010. Uses RCX.
    fn mem_check_x(&mut self, addr: R, size: u32, write: bool, bail: i32, align: bool) {
        self.naccess[self.cur] += 1;
        if align && size > 1 && self.env.align_check {
            self.b.test_ri(1, addr, 1);
            self.jcc(XC_NE, bail);
        }
        if self.env.addr_mask != 0xFFFF_FFFF {
            self.b.alu_ri(XA_AND, 4, addr, self.env.addr_mask);
        }
        let slow = self.new_label();
        let cont = self.new_label();
        let b = &mut self.b;
        b.mov_rr(4, RCX, addr);
        b.shift_ri(5, RCX, MAP_SHIFT as u8);
        b.add_rm64(RCX, xm(RCPU, OFF_MAP));
        b.test_mi8(
            xm(RCX, 0),
            if write {
                JMAP_NO_WRITE | JMAP_CODE
            } else {
                JMAP_NO_READ
            },
        );
        self.jcc(XC_NE, slow);
        if size > 1 {
            let b = &mut self.b;
            b.mov_rr(4, RCX, addr);
            b.alu_ri(XA_AND, 4, RCX, (1 << MAP_SHIFT) - 1);
            b.alu_ri(XA_CMP, 4, RCX, (1 << MAP_SHIFT) - size);
            self.jcc(XC_A, slow);
        }
        self.bind(cont);
        self.defer(slow, cont, bail, addr, size, write);
    }

    fn mem_check(&mut self, addr: R, size: u32, write: bool, bail: i32) {
        self.mem_check_x(addr, size, write, bail, true);
    }

    fn ea_index(&mut self, e: &Ea, tmp: R) -> R {
        let ix = e.index as usize;
        let xd = self.delta[ix];
        if e.index_long && xd == 0 {
            self.h(ix)
        } else {
            let h = self.h(ix);
            self.b.lea(tmp, xm(h, xd));
            if !e.index_long {
                self.b.movsx_rr(2, tmp, tmp);
            }
            tmp
        }
    }

    /// The address of a memory operand, not masked, into `dst` (RAX or
    /// RDX). (An)+ and -(An) are counted in `delta`, applied after the
    /// reads. A memory indirect operand reads its pointer here (checked:
    /// may bail).
    fn ea_address(&mut self, e: &Ea, dst: R, bail: i32) {
        if e.mem_const() {
            self.b.mov_ri(dst, e.disp);
            return;
        }
        let mut disp = e.disp as i32;
        let mut base = -1;
        let mut index = -1;
        if e.base >= 0 {
            let br = e.base as usize;
            if e.step < 0 {
                self.delta[br] += i32::from(e.step);
            }
            disp = disp.wrapping_add(self.delta[br]);
            base = self.h(br);
        }
        if e.index >= 0 && e.ind != 2 {
            index = self.ea_index(e, dst);
        }
        if base < 0 && index < 0 {
            self.b.mov_ri(dst, disp as u32);
        } else {
            self.b.lea(dst, xmi(base, index, e.scale, disp));
        }
        if e.base >= 0 && e.step > 0 {
            self.delta[e.base as usize] += i32::from(e.step);
        }
        if e.ind != 0 {
            self.mem_check(dst, 4, false, bail);
            self.load_be(4, dst, dst, 0);
            let mut index = -1;
            if e.index >= 0 && e.ind == 2 {
                index = self.ea_index(e, RCX);
            }
            if index >= 0 || e.od != 0 {
                self.b.lea(dst, xmi(dst, index, e.scale, e.od as i32));
            }
        }
    }

    /// The masked address of a memory operand of `size` bytes into `dst`,
    /// checked.
    fn mem_address(&mut self, e: &Ea, dst: R, size: u32, write: bool, bail: i32) {
        if e.mem_const() {
            // Checked when decoded; a write may still hit translated code.
            let a = e.disp & self.env.addr_mask;
            self.b.mov_ri(dst, a);
            self.naccess[self.cur] += 1;
            if e.host {
                // (what follows does not run)
                let l = self.host_label();
                self.jmp(l);
            } else if write {
                let slow = self.new_label();
                let cont = self.new_label();
                if (a & ((1 << MAP_SHIFT) - 1)) + size > 1 << MAP_SHIFT {
                    self.jmp(slow);
                } else {
                    self.b.load64(RCX, xm(RCPU, OFF_MAP));
                    self.b.test_mi8(xm(RCX, (a >> MAP_SHIFT) as i32), JMAP_CODE);
                    self.jcc(XC_NE, slow);
                }
                self.bind(cont);
                self.defer(slow, cont, bail, dst, size, true);
            }
            return;
        }
        self.ea_address(e, dst, bail);
        self.mem_check(dst, size, write, bail);
    }

    /// The source operand of an instruction, after its address checks: a
    /// register (copied if (An)+/-(An) is about to change it), memory read
    /// into RCX from the address in `addr`, or a constant.
    fn src_value(&mut self, e: &Ea, size: u32, addr: R) -> Val {
        match e.kind {
            EaKind::Dreg | EaKind::Areg => {
                if self.delta[usize::from(e.reg)] != 0 {
                    let h = self.hr(e.reg);
                    self.b.mov_rr(4, RCX, h);
                    vreg(RCX)
                } else {
                    vreg(self.hr(e.reg))
                }
            }
            EaKind::Mem => {
                self.load_be(size, RCX, addr, 0);
                vreg(RCX)
            }
            _ => vimm(e.disp),
        }
    }

    // ---- Instructions ----

    fn op_alu_ri_or_rr(&mut self, op: u8, size: u32, dst: R, v: Val) {
        if v.reg < 0 {
            self.b.alu_ri(op, size as u8, dst, v.imm);
        } else {
            self.b.alu_rr(op, size as u8, dst, v.reg);
        }
    }

    fn emit_move(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        let sz = size as u8;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        if i.dst.kind == EaKind::Mem {
            self.mem_address(&i.dst, RDX, size, true, bail);
        }
        let v = self.src_value(&i.src, size, RAX);
        self.apply_deltas();
        if i.dst.kind == EaKind::Dreg {
            let d = self.hr(i.dst.reg);
            if v.reg < 0 {
                self.b.mov_ri_sz(sz, d, v.imm);
                self.flags_const(fl, nz_of(v.imm, size));
            } else {
                self.b.mov_rr(sz, d, v.reg);
                if fl != 0 {
                    self.b.test_rr(sz, d, d);
                    self.flags_pending(fl, Pm::Logic);
                }
            }
            return;
        }
        if v.reg >= 0 && fl != 0 {
            self.b.test_rr(sz, v.reg, v.reg);
        }
        self.store_be(size, RDX, 0, v);
        if v.reg < 0 {
            self.flags_const(fl, nz_of(v.imm, size));
        } else {
            self.flags_pending(fl, Pm::Logic);
        }
    }

    fn emit_movea(&mut self, i: &Insn, bail: i32) {
        let size = i.size;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        let v = self.src_value(&i.src, size, RAX);
        self.apply_deltas();
        let d = self.hr(i.dst.reg);
        if v.reg < 0 {
            self.b
                .mov_ri(d, if size == 2 { sext(v.imm, SZ_W) } else { v.imm });
        } else if size == 2 {
            self.b.movsx_rr(2, d, v.reg);
        } else {
            self.b.mov_rr(4, d, v.reg);
        }
    }

    /// ADDA, SUBA, CMPA, and ADDQ/SUBQ to An.
    fn emit_adda(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        let mut v = self.src_value(&i.src, size, RAX);
        if v.reg < 0 {
            if size == 2 {
                v.imm = sext(v.imm, SZ_W);
            }
        } else if size == 2 {
            self.b.movsx_rr(2, RCX, v.reg);
            v.reg = RCX;
        }
        self.apply_deltas();
        let d = self.hr(i.dst.reg);
        if adda_lea(i) {
            // no flags: a LEA keeps pending ones in EFLAGS
            if v.reg < 0 {
                let imm = if i.alu == A_SUB {
                    0u32.wrapping_sub(v.imm)
                } else {
                    v.imm
                };
                self.b.lea(d, xm(d, imm as i32));
            } else {
                self.b.lea(d, xmi(d, v.reg, 0, 0));
            }
            return;
        }
        self.op_alu_ri_or_rr(x86_alu(i.alu), 4, d, v);
        if i.alu == A_CMP {
            self.flags_pending(fl, Pm::Arith);
        }
    }

    fn emit_alu(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        let op = x86_alu(i.alu);
        if i.dst.kind == EaKind::Dreg {
            if i.src.kind == EaKind::Mem {
                self.mem_address(&i.src, RAX, size, false, bail);
            }
            let v = self.src_value(&i.src, size, RAX);
            self.apply_deltas();
            let d = self.hr(i.dst.reg);
            self.op_alu_ri_or_rr(op, size, d, v);
            self.flags_pending(fl, alu_mode(i.alu));
            return;
        }
        // to memory: read, change, write
        self.mem_address(&i.dst, RDX, size, i.alu != A_CMP, bail);
        self.load_be(size, RCX, RDX, 0);
        let v = self.src_value(&i.src, size, -1);
        self.apply_deltas();
        self.op_alu_ri_or_rr(op, size, RCX, v);
        if i.alu != A_CMP {
            self.store_be(size, RDX, 0, vreg(RCX));
        }
        self.flags_pending(fl, alu_mode(i.alu));
    }

    fn emit_cmpm(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        self.mem_address(&i.src, RAX, size, false, bail);
        self.mem_address(&i.dst, RDX, size, false, bail);
        self.load_be(size, RCX, RAX, 0);
        self.b.mov_rr(4, RAX, RCX);
        self.load_be(size, RCX, RDX, 0);
        self.apply_deltas();
        self.b.alu_rr(XA_CMP, size as u8, RCX, RAX);
        self.flags_pending(fl, Pm::Arith);
    }

    fn emit_tst(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        let v = self.src_value(&i.src, size, RAX);
        self.apply_deltas();
        if v.reg < 0 {
            self.flags_const(fl, nz_of(v.imm, size));
        } else if fl != 0 {
            self.b.test_rr(size as u8, v.reg, v.reg);
            self.flags_pending(fl, Pm::Logic);
        }
    }

    fn emit_clr(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        if i.dst.kind == EaKind::Mem {
            self.mem_address(&i.dst, RDX, size, true, bail);
            self.apply_deltas();
            self.store_be(size, RDX, 0, vimm(0));
        } else {
            let d = self.hr(i.dst.reg);
            self.b.mov_ri_sz(size as u8, d, 0);
        }
        self.flags_const(fl, F_Z);
    }

    /// The operand of a read-modify-write instruction: its x86 register, or
    /// RCX loaded from the checked address in RDX.
    fn rmw_begin(&mut self, i: &Insn, bail: i32) -> R {
        if i.dst.kind != EaKind::Mem {
            return self.hr(i.dst.reg);
        }
        self.mem_address(&i.dst, RDX, i.size, true, bail);
        self.load_be(i.size, RCX, RDX, 0);
        RCX
    }

    fn rmw_end(&mut self, i: &Insn) {
        if i.dst.kind == EaKind::Mem {
            self.store_be(i.size, RDX, 0, vreg(RCX));
        }
    }

    fn emit_unary(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size as u8;
        let d = self.rmw_begin(i, bail);
        self.apply_deltas();
        match i.sub {
            U_NEG => {
                self.b.unary(XU_NEG, size, d);
                self.rmw_end(i);
                self.flags_pending(fl, Pm::ArithX);
            }
            U_NOT => {
                self.b.unary(XU_NOT, size, d);
                if fl != 0 {
                    self.b.test_rr(size, d, d);
                }
                self.rmw_end(i);
                self.flags_pending(fl, Pm::Logic);
            }
            _ => {
                // NEGX: 0 - d - X
                self.b.btx_ri(XB_BT, RSR, 4);
                self.b.mov_ri(RAX, 0);
                self.b.alu_rr(XA_SBB, size, RAX, d);
                self.b.mov_rr(size, d, RAX);
                self.rmw_end(i);
                self.flags_sticky(fl);
            }
        }
    }

    fn emit_addx(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        let op = if i.sub != 0 { XA_SBB } else { XA_ADC };
        if i.dst.kind == EaKind::Dreg {
            let d = self.hr(i.dst.reg);
            let s = self.hr(i.src.reg);
            self.b.btx_ri(XB_BT, RSR, 4);
            self.b.alu_rr(op, size as u8, d, s);
            self.flags_sticky(fl);
            return;
        }
        self.mem_address(&i.src, RAX, size, false, bail);
        self.mem_address(&i.dst, RDX, size, true, bail);
        self.load_be(size, RCX, RAX, 0);
        self.b.mov_rr(4, RAX, RCX);
        self.load_be(size, RCX, RDX, 0);
        self.apply_deltas();
        self.b.btx_ri(XB_BT, RSR, 4);
        self.b.alu_rr(op, size as u8, RCX, RAX);
        self.store_be(size, RDX, 0, vreg(RCX));
        self.flags_sticky(fl);
    }

    /// Shifts and rotates by 1-8 on Dn, or by 1 on a word in memory.
    fn emit_shift(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size as u8;
        let k = i.count;
        let mem = i.dst.kind == EaKind::Mem;
        let bit = if mem { RAX } else { RDX }; // where a C bit is kept for later
        let d = self.rmw_begin(i, bail);
        self.apply_deltas();
        let mut csrc = Fb::Cf;
        let mut vsrc = Fb::Zero;
        let mut x_eq_c = true;
        let mut pend = false;
        let b = &mut self.b;
        match i.sub {
            S_AS => {
                if k == 1 {
                    // x86's OF of a shift by one is the 68k's V for ASL, ASR
                    b.shift_r(if i.left { XS_SHL } else { XS_SAR }, size, d, 1);
                    pend = true;
                } else if i.left {
                    // V: the top k + 1 bits are not all equal (Dn only)
                    b.mov_rr(4, RAX, d);
                    if size < 4 {
                        b.shift_ri(4, RAX, 32 - 8 * size);
                    }
                    b.shift_r(XS_SAR, 4, RAX, 31 - k);
                    b.alu_ri(XA_ADD, 4, RAX, 1);
                    b.alu_ri(XA_CMP, 4, RAX, 1);
                    b.setcc(XC_A, RDX);
                    b.shift_r(XS_SHL, size, d, k);
                    vsrc = Fb::Dl;
                } else {
                    b.shift_r(XS_SAR, size, d, k);
                }
            }
            S_LS => b.shift_r(if i.left { XS_SHL } else { XS_SHR }, size, d, k),
            S_RO => {
                b.shift_r(if i.left { XS_ROL } else { XS_ROR }, size, d, k);
                b.setcc(XC_B, bit);
                b.test_rr(size, d, d);
                csrc = Fb::Dl;
                x_eq_c = false;
            }
            _ => {
                // ROX
                b.btx_ri(XB_BT, RSR, 4);
                b.shift_r(if i.left { XS_RCL } else { XS_RCR }, size, d, k);
                b.setcc(XC_B, bit);
                b.test_rr(size, d, d);
                csrc = Fb::Dl;
            }
        }
        self.rmw_end(i);
        if pend {
            self.flags_pending(fl, Pm::ArithX);
            return;
        }
        if csrc == Fb::Dl && bit != RDX {
            self.b.mov_rr(4, RDX, RAX); // no flags changed
        }
        self.flags_build(fl, csrc, vsrc, x_eq_c);
    }

    /// BTST, BCHG, BCLR, BSET.
    fn emit_bit(&mut self, i: &Insn, fl: u8, bail: i32) {
        const OPS: [u8; 4] = [XB_BT, XB_BTC, XB_BTR, XB_BTS];
        let op = OPS[usize::from(i.sub & 3)];
        if i.dst.kind == EaKind::Dreg {
            let d = self.hr(i.dst.reg);
            if i.src.kind == EaKind::Imm {
                self.b.btx_ri(op, d, (i.src.disp & 31) as u8);
            } else {
                let s = self.hr(i.src.reg);
                self.b.btx_rr(op, d, s);
            }
        } else {
            if i.dst.kind == EaKind::Imm {
                self.b.mov_ri(RCX, i.dst.disp & 0xFF);
            } else {
                self.mem_address(&i.dst, RDX, 1, i.sub != B_TST, bail);
                self.load_be(1, RCX, RDX, 0);
                self.apply_deltas();
            }
            if i.src.kind == EaKind::Imm {
                self.b.btx_ri(op, RCX, (i.src.disp & 7) as u8);
            } else {
                let s = self.hr(i.src.reg);
                self.b.mov_rr(4, RAX, s);
                self.b.alu_ri(XA_AND, 4, RAX, 7);
                self.b.btx_rr(op, RCX, RAX);
            }
            if i.sub != B_TST {
                self.store_be(1, RDX, 0, vreg(RCX));
            }
        }
        if fl & F_Z != 0 {
            let b = &mut self.b;
            b.setcc(XC_AE, RAX); // Z = !CF
            b.movzx_rr(1, RAX, RAX);
            b.shift_ri(4, RAX, 2);
            b.alu_ri(XA_AND, 4, RSR, !u32::from(F_Z));
            b.alu_rr(XA_OR, 4, RSR, RAX);
        }
    }

    /// MULU.W, MULS.W
    fn emit_mul(&mut self, i: &Insn, fl: u8, bail: i32) {
        let sign = i.sub != 0;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, 2, false, bail);
        }
        let v = self.src_value(&i.src, 2, RAX);
        self.apply_deltas();
        let d = self.hr(i.dst.reg);
        let b = &mut self.b;
        if v.reg < 0 {
            b.mov_ri(
                RCX,
                if sign {
                    sext(v.imm, SZ_W)
                } else {
                    v.imm & 0xFFFF
                },
            );
        } else if sign {
            b.movsx_rr(2, RCX, v.reg);
        } else {
            b.movzx_rr(2, RCX, v.reg);
        }
        if sign {
            b.movsx_rr(2, RAX, d);
        } else {
            b.movzx_rr(2, RAX, d);
        }
        b.imul_rr(4, RAX, RCX);
        b.mov_rr(4, d, RAX);
        if fl != 0 {
            b.test_rr(4, RAX, RAX);
            self.flags_pending(fl, Pm::Logic);
        }
    }

    /// The source of a divide, read in the first phase: a zero divisor
    /// leaves for the interpreter (the exception).
    fn div_source(&mut self, i: &Insn, size: u32, bail: i32) -> Val {
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        let v = self.src_value(&i.src, size, RAX);
        if v.reg >= 0 {
            self.b.test_rr(size as u8, v.reg, v.reg);
            self.jcc(XC_E, bail);
        }
        self.apply_deltas();
        v
    }

    /// Overflow: N and V set, Z and C clear; the registers do not change.
    fn div_end(&mut self, fl: u8, ovf: i32, done: i32) {
        self.jmp(done);
        self.bind(ovf);
        self.flags_const(fl, F_N | F_V);
        self.bind(done);
    }

    /// DIVU.W, DIVS.W
    fn emit_div(&mut self, i: &Insn, fl: u8, bail: i32) {
        let sign = i.sub != 0;
        let v = self.div_source(i, 2, bail);
        let d = self.hr(i.dst.reg);
        let ovf = self.new_label();
        let done = self.new_label();
        if !sign {
            let b = &mut self.b;
            if v.reg < 0 {
                b.mov_ri(RCX, v.imm & 0xFFFF);
            } else {
                b.movzx_rr(2, RCX, v.reg);
            }
            b.mov_rr(4, RAX, d);
            b.mov_ri(RDX, 0);
            b.unary(XU_DIV, 4, RCX);
            b.alu_ri(XA_CMP, 4, RAX, 0xFFFF);
            self.jcc(XC_A, ovf);
        } else {
            let b = &mut self.b;
            if v.reg < 0 {
                b.mov_ri(RCX, sext(v.imm, SZ_W));
                b.movsxd(RCX, RCX);
            } else {
                b.movsx64_rr(2, RCX, v.reg);
            }
            b.movsxd(RAX, d);
            b.cqo();
            b.unary(XU_IDIV, 8, RCX);
            b.movsx64_rr(2, RCX, RAX);
            b.alu_rr(XA_CMP, 8, RCX, RAX);
            self.jcc(XC_NE, ovf);
        }
        let b = &mut self.b;
        b.shift_ri(4, RDX, 16);
        b.movzx_rr(2, RAX, RAX);
        b.alu_rr(XA_OR, 4, RDX, RAX);
        b.mov_rr(4, d, RDX);
        if fl != 0 {
            b.test_rr(2, RAX, RAX);
            self.flags_from_eflags(fl, Pm::Logic);
        }
        self.div_end(fl, ovf, done);
    }

    /// MULU.L, MULS.L: the product in 64 bits; N and Z from all of it.
    fn emit_mull(&mut self, i: &Insn, fl: u8, bail: i32) {
        let ext = i.ext;
        let dl = self.h(usize::from((ext >> 12) & 7));
        let dh = self.h(usize::from(ext & 7));
        let sign = ext & 0x0800 != 0;
        let wide = ext & 0x0400 != 0;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, 4, false, bail);
        }
        let v = self.src_value(&i.src, 4, RAX);
        self.apply_deltas();
        let b = &mut self.b;
        if v.reg < 0 {
            b.mov_ri(RCX, v.imm);
        } else {
            b.mov_rr(4, RCX, v.reg);
        }
        if sign {
            b.movsxd(RCX, RCX);
            b.movsxd(RAX, dl);
        } else {
            b.mov_rr(4, RAX, dl);
        }
        b.imul_rr(8, RAX, RCX);
        if wide {
            b.mov_rr(8, RDX, RAX);
            b.shift_r(XS_SHR, 8, RDX, 32);
            b.mov_rr(4, dh, RDX);
            b.mov_rr(4, dl, RAX);
            if fl != 0 {
                b.test_rr(8, RAX, RAX);
                self.flags_pending(fl, Pm::Logic);
            }
            return;
        }
        b.mov_rr(4, dl, RAX);
        if fl == 0 {
            return;
        }
        // V: the high half is not just the extension of the low one
        if sign {
            b.movsxd(RDX, RAX);
            b.alu_rr(XA_CMP, 8, RDX, RAX);
            b.setcc(XC_NE, RDX);
        } else {
            b.mov_rr(8, RDX, RAX);
            b.shift_r(XS_SHR, 8, RDX, 32);
            b.setcc(XC_NE, RDX);
        }
        b.test_rr(8, RAX, RAX);
        self.flags_build(fl, Fb::Zero, Fb::Dl, false);
    }

    /// DIVU.L, DIVS.L, DIVUL.L, DIVSL.L (see the interpreter's).
    fn emit_divl(&mut self, i: &Insn, fl: u8, bail: i32) {
        let ext = i.ext;
        let dq = self.h(usize::from((ext >> 12) & 7));
        let dr = self.h(usize::from(ext & 7));
        let sign = ext & 0x0800 != 0;
        let wide = ext & 0x0400 != 0;
        let v = self.div_source(i, 4, bail);
        let ovf = self.new_label();
        let done = self.new_label();
        let b = &mut self.b;
        if v.reg < 0 {
            b.mov_ri(RCX, v.imm);
        } else if v.reg != RCX {
            b.mov_rr(4, RCX, v.reg);
        }
        if sign {
            b.movsxd(RCX, RCX);
        }
        if wide {
            b.mov_rr(4, RAX, dq);
            b.mov_rr(4, RDX, dr);
            b.shift_r(XS_SHL, 8, RDX, 32);
            b.alu_rr(XA_OR, 8, RAX, RDX);
        } else if sign {
            b.movsxd(RAX, dq);
        } else {
            b.mov_rr(4, RAX, dq);
        }
        if !sign {
            b.mov_ri(RDX, 0);
            b.unary(XU_DIV, 8, RCX);
            b.mov_rr(8, RCX, RAX);
            b.shift_r(XS_SHR, 8, RCX, 32);
            self.jcc(XC_NE, ovf);
        } else {
            // x86 traps on the most negative dividend divided by -1
            let m1 = self.new_label();
            let chk = self.new_label();
            self.b.alu_ri(XA_CMP, 8, RCX, 0xFFFF_FFFF);
            self.jcc(XC_E, m1);
            self.b.cqo();
            self.b.unary(XU_IDIV, 8, RCX);
            self.jmp(chk);
            self.bind(m1);
            self.b.unary(XU_NEG, 8, RAX);
            self.b.mov_ri(RDX, 0);
            self.bind(chk);
            self.b.movsxd(RCX, RAX);
            self.b.alu_rr(XA_CMP, 8, RCX, RAX);
            self.jcc(XC_NE, ovf);
        }
        let b = &mut self.b;
        b.mov_rr(4, dr, RDX);
        b.mov_rr(4, dq, RAX);
        if fl != 0 {
            b.test_rr(4, RAX, RAX);
            self.flags_from_eflags(fl, Pm::Logic);
        }
        self.div_end(fl, ovf, done);
    }

    fn emit_ext(&mut self, i: &Insn, fl: u8) {
        let d = self.hr(i.dst.reg);
        let b = &mut self.b;
        match i.sub {
            E_W => {
                b.movsx_rr(1, RCX, d);
                b.mov_rr(2, d, RCX);
                if fl != 0 {
                    b.test_rr(2, RCX, RCX);
                }
            }
            E_L => {
                b.movsx_rr(2, d, d);
                if fl != 0 {
                    b.test_rr(4, d, d);
                }
            }
            _ => {
                b.movsx_rr(1, d, d);
                if fl != 0 {
                    b.test_rr(4, d, d);
                }
            }
        }
        self.flags_pending(fl, Pm::Logic);
    }

    fn emit_scc(&mut self, i: &Insn, bail: i32) {
        let cc = i.cc;
        if i.dst.kind == EaKind::Dreg {
            let d = self.hr(i.dst.reg);
            if cc <= 1 {
                self.b.mov_ri_sz(1, d, if cc == 0 { 0xFF } else { 0 });
            } else {
                let x = self.cond(cc, i.flags_use);
                self.b.setcc(x, RDX);
                self.flags_flush();
                self.b.unary(XU_NEG, 1, RDX);
                self.b.mov_rr(1, d, RDX);
            }
            return;
        }
        self.mem_address(&i.dst, RDX, 1, true, bail);
        self.apply_deltas();
        if cc <= 1 {
            self.store_be(1, RDX, 0, vimm(if cc == 0 { 0xFF } else { 0 }));
            return;
        }
        let x = self.cond(cc, i.flags_use);
        self.b.setcc(x, RAX);
        self.b.unary(XU_NEG, 1, RAX);
        self.store_be(1, RDX, 0, vreg(RAX));
    }

    fn emit_movem(&mut self, i: &Insn, bail: i32) {
        let size = i.size;
        let total = i.mask.count_ones() * size;
        let to_regs = i.sub != 0;
        let e = if to_regs { i.src } else { i.dst };
        let mode = (i.op >> 3) & 7;
        let an = if (2..=6).contains(&mode) {
            e.base as usize
        } else {
            0
        };
        if mode == 4 {
            let h = self.h(an);
            self.b.lea(RAX, xm(h, -(total as i32)));
            self.mem_check(RAX, total, true, bail);
        } else {
            self.mem_address(&e, RAX, total, !to_regs, bail);
        }
        if mode == 3 {
            let h = self.h(an);
            self.b.lea(RDX, xm(h, total as i32));
        }
        let mut k = 0u32;
        for bit in 0..16usize {
            if i.mask & (1 << bit) == 0 {
                continue;
            }
            if mode == 4 {
                // -(An): bit 0 is A7, stored at the highest address
                let r = 15 - bit;
                k += 1;
                let v = self.reg_get(r, RCX);
                self.store_be(size, RAX, (total - k * size) as i32, vreg(v));
                continue;
            }
            let r = bit;
            let off = (k * size) as i32;
            k += 1;
            if !to_regs {
                let v = self.reg_get(r, RCX);
                self.store_be(size, RAX, off, vreg(v));
                continue;
            }
            let d = if self.h(r) >= 0 { self.h(r) } else { RCX };
            self.load_be(size, d, RAX, off);
            if size == 2 {
                self.b.movsx_rr(2, d, d);
            }
            if d == RCX {
                self.b.store(4, xm(RCPU, off_reg(r)), RCX);
            }
        }
        if mode == 4 {
            let h = self.h(an);
            self.b.lea(h, xm(h, -(total as i32)));
        } else if mode == 3 {
            let h = self.h(an);
            self.b.mov_rr(4, h, RDX);
        }
    }

    /// LINK An,#d: push An, An = SP, SP += d. LINK A7 pushes the SP from
    /// before the push.
    fn emit_link(&mut self, i: &Insn, bail: i32) {
        let sp = self.h(usize::from(ra(7)));
        let an = self.hr(i.dst.reg);
        self.b.lea(RAX, xm(sp, -4));
        self.mem_check(RAX, 4, true, bail);
        self.store_be(4, RAX, 0, vreg(an));
        self.b.lea(sp, xm(sp, -4));
        if an != sp {
            self.b.mov_rr(4, an, sp);
        }
        self.b.lea(sp, xm(sp, i.src.disp as i32));
    }

    /// UNLK An: SP = An + 4, An = (An).
    fn emit_unlk(&mut self, i: &Insn, bail: i32) {
        let sp = self.h(usize::from(ra(7)));
        let an = self.hr(i.dst.reg);
        self.b.mov_rr(4, RAX, an);
        self.mem_check(RAX, 4, false, bail);
        self.load_be(4, RCX, RAX, 0);
        self.b.lea(sp, xm(an, 4));
        self.b.mov_rr(4, an, RCX);
    }

    fn emit_pea(&mut self, i: &Insn, bail: i32) {
        let sp = self.h(usize::from(ra(7)));
        self.ea_address(&i.src, RDX, bail);
        self.b.lea(RAX, xm(sp, -4));
        self.mem_check(RAX, 4, true, bail);
        self.store_be(4, RAX, 0, vreg(RDX));
        self.b.lea(sp, xm(sp, -4));
    }

    /// An FPU instruction runs from the block only if it cannot take an
    /// exception: none pending from the one before, and none enabled in the
    /// FPCR (so this one leaves none pending either, and BSUN is off). Else
    /// it bails. Uses RAX and RCX.
    fn fpu_check(&mut self, bail: i32) {
        let b = &mut self.b;
        b.load(4, RAX, xm(RCPU, OFF_FPCR));
        b.alu_ri(XA_AND, 4, RAX, 0xFF00);
        b.load(4, RCX, xm(RCPU, OFF_FPEND));
        b.alu_rr(XA_OR, 4, RAX, RCX);
        self.jcc(XC_NE, bail);
    }

    /// Calls a function of the library with the CPU and up to three more
    /// arguments (EAX is the fourth): the x86 registers that hold 68k
    /// registers and that the callee may change are kept on the stack (an
    /// even number: RSP stays 16-byte aligned). The result is in RAX.
    fn call_out(&mut self, f: u64, a1: u32, a2: Option<u32>, rax_arg: bool) {
        const SAVED: [R; 6] = [RSI, RDI, R8, R9, R10, R11];
        let b = &mut self.b;
        for r in SAVED {
            b.push(r);
        }
        if rax_arg {
            b.mov_rr(4, RCX, RAX);
        }
        b.mov_rr(8, RDI, RCPU);
        b.mov_ri(RSI, a1);
        if let Some(a2) = a2 {
            b.mov_ri(RDX, a2);
        }
        b.mov_ri64(RAX, f);
        b.call_r(RAX);
        for r in SAVED.iter().rev() {
            b.pop(*r);
        }
    }

    /// Calls the interpreter's handler: the registers it may read are
    /// stored first, those it may change loaded after. A bit field in
    /// memory first checks the 5 bytes from the byte its offset points
    /// into.
    fn emit_call(&mut self, i: &Insn, bail: i32) {
        match i.sub {
            2 => {
                // ABCD, SBCD -(Ay),-(Ax): the source byte, then the
                // destination (one more step down when Ax is Ay)
                let y = i.src.reg;
                let x = i.dst.reg;
                let sy = if y == ra(7) { 2 } else { 1 };
                let sx = if x == ra(7) { 2 } else { 1 };
                let hy = self.hr(y);
                let hx = self.hr(x);
                self.b.lea(RAX, xm(hy, -sy));
                self.mem_check_x(RAX, 1, false, bail, false);
                self.b.lea(RDX, xm(hx, -sx - if x == y { sy } else { 0 }));
                self.mem_check_x(RDX, 1, true, bail, false);
            }
            3 => {
                // NBCD <ea>: the handler moves An itself
                if i.dst.mem_const() {
                    self.mem_address(&i.dst, RAX, 1, true, bail);
                } else {
                    self.ea_address(&i.dst, RAX, bail);
                    self.mem_check_x(RAX, 1, true, bail, false);
                }
                self.delta = [0; 16];
            }
            1 => {
                let off = (u32::from(i.ext) >> 6 & 31) >> 3;
                if i.dst.mem_const() && i.ext & 0x0800 == 0 {
                    let mut e = i.dst;
                    e.disp = e.disp.wrapping_add(off);
                    self.mem_address(&e, RAX, 5, i.write, bail);
                } else {
                    self.ea_address(&i.dst, RAX, bail);
                    if i.ext & 0x0800 != 0 {
                        let h = self.h(usize::from((i.ext >> 6) & 7));
                        self.b.mov_rr(4, RCX, h);
                        self.b.shift_r(XS_SAR, 4, RCX, 3);
                        self.b.alu_rr(XA_ADD, 4, RAX, RCX);
                    } else if off != 0 {
                        self.b.alu_ri(XA_ADD, 4, RAX, off);
                    }
                    self.mem_check(RAX, 5, i.write, bail);
                }
            }
            _ => {}
        }
        self.store_regs();
        self.b.store(2, xm(RCPU, OFF_SR), RSR);
        self.b.store_imm(4, xm(RCPU, OFF_PC), i.pc.wrapping_add(2));
        self.call_out(self.env.call_handler, u32::from(i.op), None, false);
        for r in 0..16 {
            if i.wregs & (1 << r) != 0 && self.h(r) >= 0 {
                let h = self.h(r);
                self.b.load(4, h, xm(RCPU, off_reg(r)));
            }
        }
        self.b.movzx_load(2, RSR, xm(RCPU, OFF_SR));
    }

    /// TAS: N and Z of the byte, then bit 7 set.
    fn emit_tas(&mut self, i: &Insn, fl: u8, bail: i32) {
        let d = self.rmw_begin(i, bail);
        self.apply_deltas();
        self.b.mov_rr(4, RAX, d);
        self.b.alu_ri(XA_OR, 1, d, 0x80);
        self.rmw_end(i);
        if fl != 0 {
            self.b.test_rr(1, RAX, RAX);
            self.flags_pending(fl, Pm::Logic);
        }
    }

    /// CHK: Dn < 0 or Dn > the bound takes the exception (the
    /// interpreter's, after a bail). Otherwise the 68000 and 68010 set Z
    /// from Dn and clear N, V and C; later models change nothing.
    fn emit_chk(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        if i.src.kind == EaKind::Mem {
            self.mem_address(&i.src, RAX, size, false, bail);
        }
        let mut v = self.src_value(&i.src, size, RAX);
        let dn = self.hr(i.dst.reg);
        if size == 2 {
            self.b.movsx_rr(2, RAX, dn);
            if v.reg >= 0 {
                self.b.movsx_rr(2, RCX, v.reg);
                v.reg = RCX;
            } else {
                v.imm = sext(v.imm, SZ_W);
            }
        } else {
            self.b.mov_rr(4, RAX, dn);
        }
        self.b.test_rr(4, RAX, RAX);
        self.jcc(XC_S, bail);
        self.op_alu_ri_or_rr(XA_CMP, 4, RAX, v);
        self.jcc(XC_G, bail);
        self.apply_deltas();
        if fl != 0 {
            self.b.test_rr(4, RAX, RAX);
            self.flags_pending(fl, Pm::Logic);
        }
    }

    /// CAS Dc,Du,<ea>: flags of <ea> - Dc; equal, Du goes to <ea>; else
    /// <ea> goes to Dc. (Both paths keep EFLAGS.)
    fn emit_cas(&mut self, i: &Insn, fl: u8, bail: i32) {
        let size = i.size;
        self.mem_address(&i.dst, RDX, size, true, bail);
        self.load_be(size, RCX, RDX, 0);
        self.apply_deltas();
        let dc = self.h(usize::from(i.ext & 7));
        let du = self.h(usize::from((i.ext >> 6) & 7));
        let ne = self.new_label();
        let done = self.new_label();
        self.b.alu_rr(XA_CMP, size as u8, RCX, dc);
        self.jcc(XC_NE, ne);
        self.store_be(size, RDX, 0, vreg(du));
        self.jmp(done);
        self.bind(ne);
        self.b.mov_rr(size as u8, dc, RCX);
        self.bind(done);
        self.flags_pending(fl, Pm::Arith);
    }

    /// MOVEP: the bytes of Dn, high first, at every other address.
    fn emit_movep(&mut self, i: &Insn, bail: i32) {
        let n = i.size;
        let dn = self.hr(i.src.reg);
        let base = self.h(i.dst.base as usize);
        self.b.lea(RDX, xm(base, i.dst.disp as i32));
        self.mem_check_x(RDX, 2 * n - 1, i.sub != 0, bail, false);
        let b = &mut self.b;
        for k in 0..n {
            let m = xmi(RMEM, RDX, 0, (2 * k) as i32);
            if i.sub != 0 {
                b.mov_rr(4, RCX, dn);
                if k + 1 < n {
                    b.shift_ri(5, RCX, (8 * (n - 1 - k)) as u8);
                }
                b.store(1, m, RCX);
            } else if k == 0 {
                b.movzx_load(1, RAX, m);
            } else {
                b.movzx_load(1, RCX, m);
                b.shift_ri(4, RAX, 8);
                b.alu_rr(XA_OR, 4, RAX, RCX);
            }
        }
        if i.sub == 0 {
            b.mov_rr(n as u8, dn, RAX);
        }
    }

    /// An FPU instruction: its operand addressed and checked (An moved
    /// here), or a Dn read or written here; the rest in fpu_jit_op().
    fn emit_fpu(&mut self, i: &Insn, bail: i32) {
        let out = i.ext >> 13 == 3;
        self.fpu_check(bail);
        if i.dst.kind == EaKind::Mem {
            self.mem_address(&i.dst, RAX, i.size, i.write, bail);
            self.apply_deltas();
        } else if i.dst.kind == EaKind::Dreg && !out {
            let h = self.hr(i.dst.reg);
            self.b.mov_rr(4, RAX, h);
        }
        let cmd_ea = u32::from(i.ext) | u32::from(i.op & 0x3F) << 16;
        self.call_out(self.env.fpu_op, i.pc, Some(cmd_ea), true);
        if i.dst.kind == EaKind::Dreg && out {
            let h = self.hr(i.dst.reg);
            self.b.mov_rr(i.size as u8, h, RAX);
        }
    }

    /// FBcc: the condition from fpu_jit_branch(), then as Bcc.
    fn emit_fbcc(&mut self, i: &Insn, bail: i32) {
        self.fpu_check(bail);
        self.call_out(self.env.fpu_branch, i.pc, Some(i.cc), false);
        let taken = self.new_label();
        self.b.test_rr(4, RAX, RAX);
        self.jcc(XC_NE, taken);
        let f = self.fs_save();
        self.exit_chain(i.pc.wrapping_add(i.len));
        self.bind(taken);
        self.fs_restore(f);
        self.exit_branch(i.target);
    }

    fn emit_bcc(&mut self, i: &Insn) {
        let next = i.pc.wrapping_add(i.len);
        if i.cc == 0 {
            self.exit_branch(i.target);
            return;
        }
        let taken = self.new_label();
        let x = self.cond(i.cc, i.flags_use);
        self.flags_settle();
        self.jcc(x, taken);
        let f = self.fs_save();
        self.exit_chain(next);
        self.bind(taken);
        self.fs_restore(f);
        self.exit_branch(i.target);
    }

    fn emit_dbcc(&mut self, i: &Insn) {
        let next = i.pc.wrapping_add(i.len);
        if i.cc == 0 {
            // DBT: never loops
            self.exit_chain(next);
            return;
        }
        let out_true = self.new_label();
        let m1 = self.new_label();
        let x = if i.cc != 1 {
            self.cond(i.cc, i.flags_use)
        } else {
            0
        };
        self.flags_settle();
        let f = self.fs_save();
        if i.cc != 1 {
            self.jcc(x, out_true);
        }
        self.flags_flush();
        // Dn.W - 1: CF when it was 0, i.e. the counter is now -1
        let d = self.hr(i.dst.reg);
        self.b.alu_ri(XA_SUB, 2, d, 1);
        let g = self.fs_save();
        self.jcc(XC_B, m1);
        self.exit_branch(i.target);
        self.bind(m1);
        self.fs_restore(g);
        self.exit_chain(next);
        if i.cc != 1 {
            self.bind(out_true);
            self.fs_restore(f);
            self.exit_chain(next);
        }
    }

    /// BSR and JSR: push the return address (checked: a long at A7 - 4).
    fn push_return(&mut self, i: &Insn, bail: i32) {
        let sp = self.h(usize::from(ra(7)));
        self.b.lea(RAX, xm(sp, -4));
        self.mem_check(RAX, 4, true, bail);
        self.store_be(4, RAX, 0, vimm(i.pc.wrapping_add(i.len)));
        self.b.lea(sp, xm(sp, -4));
    }

    fn emit_jump(&mut self, i: &Insn, bail: i32) {
        match i.kind {
            Kind::Bsr => {
                self.push_return(i, bail);
                self.exit_branch(i.target);
            }
            Kind::Jmp | Kind::Jsr => {
                if i.src.mem_const() {
                    if i.kind == Kind::Jsr {
                        self.push_return(i, bail);
                    }
                    self.exit_branch(i.src.disp);
                    return;
                }
                self.ea_address(&i.src, RDX, bail);
                self.b.test_ri(1, RDX, 1); // odd: address error
                self.jcc(XC_NE, bail);
                if i.kind == Kind::Jsr {
                    self.push_return(i, bail);
                }
                self.exit_indirect(RDX);
            }
            _ => {
                // RTS
                let sp = self.h(usize::from(ra(7)));
                self.b.mov_rr(4, RAX, sp);
                self.mem_check(RAX, 4, false, bail);
                self.load_be(4, RDX, RAX, 0);
                self.b.test_ri(1, RDX, 1);
                self.jcc(XC_NE, bail);
                self.b.lea(sp, xm(sp, 4));
                self.exit_indirect(RDX);
            }
        }
    }

    fn emit_insn(&mut self, k: usize) {
        let i = self.ins[k];
        let fl = i.flags_def & self.live_out[k];
        let mut bail = -1;
        self.cur = k;
        if i.may_bail {
            bail = self.new_label();
            self.bail[k] = bail;
        }
        self.delta = [0; 16];
        let via_eflags = reads_eflags(&i) && self.pend.mode != Pm::None;
        if !reads_eflags(&i) && !keeps_eflags(&i) {
            self.flags_flush();
        }
        let need = i.flags_use;
        if need != 0 && !via_eflags {
            // R14 must have what the instruction reads, but a condition can
            // also take all of it from the record.
            let miss = need & !self.own;
            if !(reads_cond(&i) && miss == need && miss & !self.rec == 0) {
                self.flags_need(need);
            }
        }
        self.own_at[k] = self.own;
        match i.kind {
            Kind::Nop => {}
            Kind::Moveq => {
                let d = self.hr(i.dst.reg);
                self.b.mov_ri(d, i.src.disp);
                self.flags_const(fl, nz_of(i.src.disp, 4));
            }
            Kind::Move => self.emit_move(&i, fl, bail),
            Kind::Movea => self.emit_movea(&i, bail),
            Kind::Lea => {
                self.ea_address(&i.src, RAX, bail);
                let d = self.hr(i.dst.reg);
                self.b.mov_rr(4, d, RAX);
            }
            Kind::Pea => self.emit_pea(&i, bail),
            Kind::Adda => self.emit_adda(&i, fl, bail),
            Kind::Alu => self.emit_alu(&i, fl, bail),
            Kind::Cmpm => self.emit_cmpm(&i, fl, bail),
            Kind::Tst => self.emit_tst(&i, fl, bail),
            Kind::Clr => self.emit_clr(&i, fl, bail),
            Kind::Unary => self.emit_unary(&i, fl, bail),
            Kind::Addx => self.emit_addx(&i, fl, bail),
            Kind::Shift => self.emit_shift(&i, fl, bail),
            Kind::Bit => self.emit_bit(&i, fl, bail),
            Kind::Mul => self.emit_mul(&i, fl, bail),
            Kind::Div => self.emit_div(&i, fl, bail),
            Kind::Mull => self.emit_mull(&i, fl, bail),
            Kind::Divl => self.emit_divl(&i, fl, bail),
            Kind::Ext => self.emit_ext(&i, fl),
            Kind::Swap => {
                let d = self.hr(i.dst.reg);
                self.b.shift_r(XS_ROL, 4, d, 16);
                if fl != 0 {
                    self.b.test_rr(4, d, d);
                }
                self.flags_pending(fl, Pm::Logic);
            }
            Kind::Exg => {
                if i.src.reg != i.dst.reg {
                    let s = self.hr(i.src.reg);
                    let d = self.hr(i.dst.reg);
                    self.b.xchg_rr(s, d);
                }
            }
            Kind::Scc => self.emit_scc(&i, bail),
            Kind::Movem => self.emit_movem(&i, bail),
            Kind::Link => self.emit_link(&i, bail),
            Kind::Unlk => self.emit_unlk(&i, bail),
            Kind::Tas => self.emit_tas(&i, fl, bail),
            Kind::Chk => self.emit_chk(&i, fl, bail),
            Kind::Cas => self.emit_cas(&i, fl, bail),
            Kind::Movep => self.emit_movep(&i, bail),
            Kind::Call => self.emit_call(&i, bail),
            Kind::Fbcc => self.emit_fbcc(&i, bail),
            Kind::Fpu => self.emit_fpu(&i, bail),
            Kind::Bcc => self.emit_bcc(&i),
            Kind::Dbcc => self.emit_dbcc(&i),
            Kind::Bsr | Kind::Jmp | Kind::Jsr | Kind::Rts => self.emit_jump(&i, bail),
        }
        // What the instruction wrote into R14 (what it left pending is in
        // `pend`; a handler call reloads all of SR).
        if i.kind == Kind::Call && i.flags_use == F_ALL {
            self.own = F_ALL;
            self.rec = 0;
        } else if fl != 0 {
            let r14 = fl
                & !if self.pend.mode != Pm::None {
                    self.pend.mask
                } else {
                    0
                };
            self.own |= r14;
            self.rec &= !r14;
        }
    }

    // ---- Blocks ----

    /// The out-of-line half of the access checks: asks the access check.
    fn emit_deferred(&mut self) {
        for k in 0..self.defer.len() {
            let d = self.defer[k];
            self.bind(d.slow);
            self.b.mov_rr(4, RCX, d.addr);
            self.b.push_imm(d.size_write as i32);
            self.call_abs(self.env.check_off);
            self.b.lea64(RSP, xm(RSP, 8));
            self.b.alu_ri(XA_CMP, 4, RCX, 1);
            self.jcc(XC_E, d.cont);
            self.jcc(XC_B, d.bail);
            self.jmp(d.host);
        }
    }

    /// An access to a host page (or a ROM write) in instruction k: R14 and
    /// the record made to agree, the registers and SR stored, the
    /// instructions before counted; then the interpreter runs instruction k
    /// (the host step) and the block goes on to the next one through a
    /// chained jump, or leaves for the dispatcher. Either way the state is
    /// in the CPU structure, so nothing is stored again.
    fn emit_host_path(&mut self, k: usize) {
        let i = self.ins[k];
        let next = i.pc.wrapping_add(i.len);
        self.bind(self.hostp[k]);
        self.pend = NO_PEND;
        self.own = self.own_at[k];
        self.flags_exit();
        self.store_regs();
        self.b.store(2, xm(RCPU, OFF_SR), RSR);
        self.b.store_imm(4, xm(RCPU, OFF_PC), i.pc);
        self.count(k);
        // What can fault in the interpreter besides the access checked:
        // other memory operands, a jump to an odd address, and the frame of
        // an exception (division by zero, CHK) pushed to a missing page.
        let may_fault = self.naccess[k] > 1
            || i.ends_block
            || matches!(i.kind, Kind::Div | Kind::Divl | Kind::Chk);
        let b = &mut self.b;
        b.mov_rr(8, RDI, RCPU);
        b.mov_ri(RSI, next);
        b.mov_ri(RDX, u32::from(may_fault));
        b.mov_ri64(RAX, self.env.host_step);
        b.call_r(RAX);
        b.movzx_load(2, RSR, xm(RCPU, OFF_SR));
        let leave = self.new_label();
        self.b.test_rr(4, RAX, RAX);
        self.jcc(XC_NE, leave);
        let at = self.b.jmp32(); // chained to the block at `next`
        self.b.mov_ri(RAX, next);
        self.b.mov_ri(RCX, EXIT_CHAIN);
        self.b.mov_ri(RDX, (self.base + at) as u32);
        self.jmp_abs(self.env.exit_off);
        self.bind(leave);
        self.b.load(4, RAX, xm(RCPU, OFF_PC));
        self.b.mov_ri(RCX, EXIT_NORMAL);
        self.jmp_abs(self.env.exit_off);
    }

    /// Translates `ins` (at least one instruction, at most the registers
    /// the pool has) into code to be placed at offset `base` of the code
    /// memory. Returns how many bytes it has (code() gives them), or None
    /// if a limit was hit.
    pub fn translate(&mut self, ins: &[Insn], base: usize) -> Option<usize> {
        let n = ins.len();
        self.ins.clear();
        self.ins.extend_from_slice(ins);
        self.n = n;
        self.base = base;
        self.b.b.clear();
        self.label_pos.clear();
        self.fix.clear();
        self.defer.clear();
        self.failed = false;
        self.pend = NO_PEND;
        self.own = if self.env.lazy { 0 } else { F_ALL };
        self.rec = 0;
        self.host = [-1; 16];
        self.used = 0;
        self.written = 0;
        self.delta = [0; 16];
        self.naccess = [0; MAX_INSNS];

        // Registers, by order of first use.
        let mut next = 0;
        for i in ins {
            for r in 0..16 {
                if i.regs & (1 << r) != 0 && self.host[r] < 0 {
                    if next >= HOST_REGS {
                        return None; // the block former keeps to 9
                    }
                    self.host[r] = HOST_POOL[next];
                    next += 1;
                }
            }
            self.used |= i.regs;
        }
        for i in ins {
            self.written |= i.wregs & self.used;
        }

        // Flags: which ones are read after each instruction (backwards).
        let mut live = F_ALL;
        for k in (0..n).rev() {
            let i = &ins[k];
            self.live_out[k] = live;
            live = (live & !i.flags_def) | i.flags_use;
            if i.may_bail {
                live = F_ALL;
            }
        }
        self.cyc_before[0] = 0;
        for (k, i) in ins.iter().enumerate() {
            self.cyc_before[k + 1] = self.cyc_before[k] + i.cycles;
            self.bail[k] = -1;
            self.hostp[k] = -1;
        }

        self.tail = self.new_label();
        self.loop_ = self.new_label();
        self.entry_out = self.new_label();

        // Entry: enough budget and no attention needed, then the registers
        // (SR is in R14 already).
        self.b.alu_mi(XA_CMP, 8, xm(RCPU, OFF_BUDGET), n as i32);
        self.jcc(XC_B, self.entry_out);
        self.b.load64(RAX, xm(RCPU, OFF_ATTN));
        self.b.alu_mi(XA_CMP, 4, xm(RAX, 0), 0);
        self.jcc(XC_NE, self.entry_out);
        for r in 0..16 {
            if self.used & (1 << r) != 0 {
                let h = self.h(r);
                self.b.load(4, h, xm(RCPU, off_reg(r)));
            }
        }
        self.bind(self.loop_);

        for k in 0..n {
            self.emit_insn(k);
        }
        let last = ins[n - 1];
        if !last.ends_block {
            self.exit_chain(last.pc.wrapping_add(last.len));
        }

        self.emit_deferred();
        for k in 0..n {
            if self.hostp[k] >= 0 {
                self.emit_host_path(k);
            }
        }

        // The bail exits: PC of the instruction, what came before counted.
        for (k, i) in ins.iter().enumerate() {
            if self.bail[k] < 0 {
                continue;
            }
            self.bind(self.bail[k]);
            self.pend = NO_PEND;
            self.own = self.own_at[k];
            self.flags_exit();
            self.b.mov_ri(RAX, i.pc);
            self.count(k);
            self.b.mov_ri(RCX, EXIT_BAIL);
            self.jmp(self.tail);
        }

        // Not entered: nothing done.
        self.bind(self.entry_out);
        self.b.mov_ri(RAX, ins[0].pc);
        self.b.mov_ri(RCX, EXIT_NORMAL);
        self.jmp_abs(self.env.exit_off);

        // The common tail: EAX the next PC, ECX the exit code.
        self.bind(self.tail);
        self.exit_stores();
        self.jmp_abs(self.env.exit_off);

        if self.failed {
            return None;
        }
        for k in 0..self.fix.len() {
            let (at, l) = self.fix[k];
            let to = self.label_pos[l as usize];
            if to < 0 {
                return None;
            }
            self.b.link(at, to);
        }
        Some(self.b.pos())
    }

    /// The code of the last block translated.
    pub fn code(&self) -> &[u8] {
        &self.b.b
    }
}

/// Where flags_build() takes C or V from.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Fb {
    Cf,
    Dl,
    Zero,
}

fn nz_of(v: u32, size: u32) -> u8 {
    let v = v & sz_mask(size);
    (if v == 0 { F_Z } else { 0 }) | if v & sz_msb(size) != 0 { F_N } else { 0 }
}

fn x86_alu(alu: u8) -> u8 {
    match alu {
        A_ADD => XA_ADD,
        A_SUB => XA_SUB,
        A_CMP => XA_CMP,
        A_AND => XA_AND,
        A_OR => XA_OR,
        _ => XA_XOR,
    }
}

fn alu_mode(alu: u8) -> Pm {
    match alu {
        A_AND | A_OR | A_EOR => Pm::Logic,
        A_CMP => Pm::Arith,
        _ => Pm::ArithX,
    }
}

/// ADDA/SUBA that become a LEA.
fn adda_lea(i: &Insn) -> bool {
    i.kind == Kind::Adda
        && i.alu != A_CMP
        && (i.src.kind == EaKind::Imm
            || (matches!(i.src.kind, EaKind::Dreg | EaKind::Areg) && i.alu == A_ADD))
}

/// Whether an instruction can use pending flags as they are in EFLAGS (it
/// does not change EFLAGS before reading them).
fn reads_eflags(i: &Insn) -> bool {
    matches!(i.kind, Kind::Bcc | Kind::Dbcc) || (i.kind == Kind::Scc && i.dst.kind == EaKind::Dreg)
}

/// Whether an instruction leaves EFLAGS alone (and never bails): pending
/// flags can wait across it.
fn keeps_eflags(i: &Insn) -> bool {
    match i.kind {
        Kind::Nop | Kind::Exg => true,
        Kind::Lea => i.src.ind == 0,
        Kind::Movea => i.src.kind != EaKind::Mem,
        Kind::Adda => adda_lea(i),
        _ => false,
    }
}

/// Whether an instruction reads its condition with cond(), which can take
/// the flags back from the record.
fn reads_cond(i: &Insn) -> bool {
    matches!(i.kind, Kind::Bcc | Kind::Dbcc | Kind::Scc)
}

// ---- The trampoline and the shared stubs ---------------------------------------

/// Where the shared code is, from the start of the code memory.
pub struct Stubs {
    pub exit_off: usize,
    pub check_off: usize,
    pub ccr_off: usize,
}

fn align16(b: &mut XBuf) {
    while b.pos() & 15 != 0 {
        b.byte(0xCC);
    }
}

/// The code at the start of the code memory: enter(cpu, mem, code) saves
/// the callee-saved registers, sets RBX and R15 and jumps to the block. The
/// common exit stores the PC and returns ECX | EDX << 32. The check stub
/// calls the access check (`access_check`) with the address in ECX and the
/// size and direction pushed before the call, keeps every register but
/// RCX, and returns the answer in ECX. The record merge stub (ECX: the CCR
/// bits to keep in R14) takes N Z V C (those the record gives) and X from
/// the record, and empties it; it changes RAX, RCX, RDX and the flags.
pub fn init(b: &mut XBuf, access_check: u64) -> Stubs {
    const SAVED: [R; 6] = [RBX, RBP, R12, R13, R14, R15];
    for r in SAVED {
        b.push(r);
    }
    // RSP 16-byte aligned inside blocks (an even number of pushes)
    b.rsp_adjust(-8);
    b.mov_rr(8, RCPU, RDI);
    b.mov_rr(8, RMEM, RSI);
    b.movzx_load(2, RSR, xm(RCPU, OFF_SR));
    b.jmp_r(RDX);

    align16(b);
    let epilogue = b.pos();
    b.rsp_adjust(8);
    for r in SAVED.iter().rev() {
        b.pop(*r);
    }
    b.ret();

    align16(b);
    let exit_off = b.pos();
    b.store(4, xm(RCPU, OFF_PC), RAX);
    b.store(2, xm(RCPU, OFF_SR), RSR);
    b.mov_rr(4, RAX, RCX);
    b.shift_r(XS_SHL, 8, RDX, 32);
    b.alu_rr(XA_OR, 8, RAX, RDX);
    let at = b.jmp32();
    b.link(at, epilogue as i64);

    align16(b);
    let check_off = b.pos();
    const KEEP: [R; 9] = [RAX, RDX, RSI, RDI, R8, R9, R10, R11, RBP];
    for r in KEEP {
        b.push(r);
    }
    b.mov_rr(8, RBP, RSP);
    b.alu_ri(XA_AND, 8, RSP, 0xFFFF_FFF0);
    b.mov_rr(8, RDI, RCPU);
    b.mov_rr(4, RSI, RCX);
    b.load(4, RDX, xm(RBP, 80));
    b.mov_ri64(RAX, access_check);
    b.call_r(RAX);
    b.mov_rr(4, RCX, RAX);
    b.mov_rr(8, RSP, RBP);
    for r in KEEP.iter().rev() {
        b.pop(*r);
    }
    b.ret();

    align16(b);
    let ccr_off = b.pos();
    b.push(RSI);
    b.push(RDI);
    b.mov_rr(4, RSI, RCX);
    b.unary(XU_NOT, 4, RSI); // ESI: what to take
    b.movzx_load(2, RAX, xm(RCPU, OFF_CCR));
    b.mov_rr(4, RCX, RAX); // N Z: bits 15, 14
    b.shift_ri(5, RCX, 12);
    b.alu_ri(XA_AND, 4, RCX, u32::from(F_N | F_Z));
    b.mov_rr(4, RDX, RAX); // V: bit 0
    b.alu_ri(XA_AND, 4, RDX, 1);
    b.alu_rr(XA_ADD, 4, RDX, RDX);
    b.alu_rr(XA_OR, 4, RCX, RDX);
    b.mov_rr(4, RDX, RAX); // C: bit 8
    b.shift_ri(5, RDX, 8);
    b.alu_ri(XA_AND, 4, RDX, 1);
    b.alu_rr(XA_OR, 4, RCX, RDX);
    b.movzx_load(1, RDI, xm(RCPU, OFF_CCR + 2));
    b.alu_rr(XA_AND, 4, RDI, RSI); // EDI: bits taken
    b.movzx_load(1, RAX, xm(RCPU, OFF_CCR + 3));
    b.test_rr(4, RAX, RAX);
    let no_x = b.jcc32(XC_E);
    b.test_ri(4, RSI, u32::from(F_X));
    let no_x2 = b.jcc32(XC_E);
    b.alu_ri(XA_AND, 4, RAX, 1);
    b.shift_ri(4, RAX, 4);
    b.alu_rr(XA_OR, 4, RCX, RAX);
    b.alu_ri(XA_OR, 4, RDI, u32::from(F_X));
    let here = b.pos() as i64;
    b.link(no_x, here);
    b.link(no_x2, here);
    b.alu_rr(XA_AND, 4, RCX, RDI);
    b.unary(XU_NOT, 4, RDI);
    b.alu_rr(XA_AND, 4, RSR, RDI);
    b.alu_rr(XA_OR, 4, RSR, RCX);
    b.store_imm(2, xm(RCPU, OFF_CCR + 2), 0);
    b.pop(RDI);
    b.pop(RSI);
    b.ret();

    align16(b);
    Stubs {
        exit_off,
        check_off,
        ccr_off,
    }
}
