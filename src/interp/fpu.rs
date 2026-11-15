// SPDX-License-Identifier: MPL-2.0
//! The floating-point unit: the MC68881/MC68882 next to a 68020 or 68030,
//! and the FPU built into the 68040 and 68060 (MC68881UM, M68000PRM chapter
//! 5, MC68040UM chapter 9, MC68060UM chapter 6). Coprocessor 1 of line F:
//!
//! ```text
//!   F200+ea  general: arithmetic, FMOVE in and out, FMOVECR, FMOVEM
//!   F240+ea  FScc, FDBcc (F248+n), FTRAPcc (F27A-F27C)
//!   F280/F2C0  FBcc with a 16 or 32-bit displacement (FNOP is FBF.W 0)
//!   F300+ea  FSAVE, F340+ea FRESTORE (privileged)
//! ```
//!
//! The arithmetic is in crate::fpx. Each instruction that computes clears
//! the exception byte of the FPSR, sets the bits it raises and ORs them into
//! the accrued byte. FPIAR gets the address of the instructions that can
//! raise an exception: the general ones but FMOVEM, and the conditional ones
//! (BSUN).
//!
//! Exceptions: an exception raised and enabled in the FPCR is taken, as on
//! the 68881, before the next floating-point instruction (other than FSAVE,
//! FRESTORE and the moves of control registers): a format $0 frame with the
//! address of that instruction, through vector 48 (BSUN) to 54 (SNAN), the
//! highest priority first. With SNAN, OPERR or DZ enabled the destination
//! is left as it was; with OVFL, UNFL or INEX it gets the normal result.
//! BSUN (a predicate 0x10-0x1F that meets a NaN) is taken at once, before
//! the conditional instruction runs. FSAVE puts a pending exception in the
//! frame and FRESTORE takes it back.
//!
//! The 68040 and 68060 leave the transcendental functions, FMOVECR,
//! FGETEXP, FGETMAN, FMOD, FREM, FSCALE, FSGLMUL and FSGLDIV (and FINT,
//! FINTRZ on the 68040; FScc, FDBcc and FTRAPcc on the 68060) to software:
//! line F, vector 11, as any unimplemented instruction (the host's line F
//! function first). The packed decimal format and denormalized operands,
//! also software on those chips, are executed here.

use super::control::op_line_f;
use super::{OpTable, SZ_B, SZ_L, SZ_W, sext, sz_mask};
use crate::cpu::{ACC_DATA, ACC_PROG, Cpu, Fpu, Model, Res, VEC_FORMAT_ERROR, VEC_TRAPV};
use crate::fpx::{self, Env, Fpx, Func};

const VEC_FP_BSUN: u32 = 48;
const VEC_FP_INEX: u32 = 49;
const VEC_FP_DZ: u32 = 50;
const VEC_FP_UNFL: u32 = 51;
const VEC_FP_OPERR: u32 = 52;
const VEC_FP_OVFL: u32 = 53;
const VEC_FP_SNAN: u32 = 54;

const FPCR_MASK: u32 = 0x0000_FFF0;
const FPSR_MASK: u32 = 0x0FFF_FFF8;
const FPSR_CC: u32 = 0x0F00_0000;
const FPSR_QUOT: u32 = 0x00FF_0000;
const FPSR_EXC: u32 = 0x0000_FF00;

impl Cpu {
    /// The FPU after a reset: registers NaN (all ones), FPCR, FPSR and FPIAR
    /// 0, null state.
    pub fn fpu_reset(&mut self) {
        self.r.fp = [fpx::default_nan(); 8];
        self.r.fpcr = 0;
        self.r.fpsr = 0;
        self.r.fpiar = 0;
        self.fpu_null = true;
        self.fpu_pending = 0;
    }

    fn internal_fpu(&self) -> bool {
        self.fpu == Fpu::Internal
    }

    /// An encoding this FPU does not execute: line F, as if the instruction
    /// had not started (the host sees the PC after the opcode).
    fn fline(&mut self, op: u16) -> Res {
        self.r.pc = self.insn_pc.wrapping_add(2);
        op_line_f(self, op)
    }

    /// Every floating-point instruction but FSAVE and FRESTORE starts here.
    /// Takes an exception left pending by the previous one (when `pending`
    /// is true); false if it did.
    fn fpu_begin(&mut self, pending: bool) -> Res<bool> {
        self.fpu_null = false;
        if pending && self.fpu_pending != 0 {
            let v = self.fpu_pending;
            self.fpu_pending = 0;
            self.exception_group1(v)?;
            return Ok(false);
        }
        Ok(true)
    }

    fn fpu_env(&self) -> Env {
        const PRECS: [u32; 4] = [64, 24, 53, 64];
        Env {
            mode: (self.r.fpcr >> 4) & 3,
            prec: PRECS[((self.r.fpcr >> 6) & 3) as usize],
            flags: 0,
        }
    }

    /// The end of an instruction that computes: exception and accrued
    /// bytes, an enabled exception left pending. Returns whether the
    /// destination may be written.
    fn fpu_status(&mut self, flags: u32) -> bool {
        self.r.fpsr = (self.r.fpsr & !FPSR_EXC) | (flags & FPSR_EXC) | fpx::accrued(flags);
        let en = flags & self.r.fpcr & 0xFF00;
        if en != 0 {
            self.fpu_pending = trap_vector(en);
        }
        en & (fpx::SNAN | fpx::OPERR | fpx::DZ) == 0
    }

    fn set_fcc(&mut self, cc: u32) {
        self.r.fpsr = (self.r.fpsr & !FPSR_CC) | cc;
    }
}

/// The exception vector for enabled exceptions, by priority.
fn trap_vector(en: u32) -> u32 {
    if en & fpx::BSUN != 0 {
        VEC_FP_BSUN
    } else if en & fpx::SNAN != 0 {
        VEC_FP_SNAN
    } else if en & fpx::OPERR != 0 {
        VEC_FP_OPERR
    } else if en & fpx::OVFL != 0 {
        VEC_FP_OVFL
    } else if en & fpx::UNFL != 0 {
        VEC_FP_UNFL
    } else if en & fpx::DZ != 0 {
        VEC_FP_DZ
    } else {
        VEC_FP_INEX
    }
}

// ---- Operands in memory --------------------------------------------------

// Formats of the source/destination specifier: L S X P W D B (7: packed
// with a dynamic k-factor on output, FMOVECR on input).
const F_L: u32 = 0;
const F_S: u32 = 1;
const F_X: u32 = 2;
const F_P: u32 = 3;
const F_W: u32 = 4;
const F_D: u32 = 5;
const F_B: u32 = 6;
const FMT_SIZE: [u32; 8] = [4, 4, 12, 12, 2, 8, 1, 12];

impl Cpu {
    /// Reads `size` bytes (1, 2, 4, 8, 12) at addr as longs (a byte or word
    /// in the low bits of the first).
    fn read_mem(&mut self, addr: u32, size: u32, acc: u32) -> Res<[u32; 3]> {
        let mut w = [0u32; 3];
        match size {
            1 => w[0] = self.read8(addr, acc)?,
            2 => w[0] = self.read16(addr, acc)?,
            _ => {
                for i in 0..size / 4 {
                    w[i as usize] = self.read32(addr.wrapping_add(4 * i), acc)?;
                }
            }
        }
        Ok(w)
    }

    fn write_mem(&mut self, addr: u32, size: u32, w: &[u32; 3]) -> Res {
        match size {
            1 => self.write8(addr, w[0]),
            2 => self.write16(addr, w[0]),
            _ => {
                for i in 0..size / 4 {
                    self.write32(addr.wrapping_add(4 * i), w[i as usize])?;
                }
                Ok(())
            }
        }
    }
}

/// Whether mode/reg can be the source of format fmt: Dn only for the
/// formats of 4 bytes or less but not packed, no An.
fn src_valid(mode: u32, reg: u32, fmt: u32) -> bool {
    match mode {
        0 => matches!(fmt, F_L | F_S | F_W | F_B),
        1 => false,
        7 => reg <= 4,
        _ => true,
    }
}

/// Data alterable for a destination; Dn for the short formats.
fn dst_valid(mode: u32, reg: u32, fmt: u32) -> bool {
    match mode {
        0 => matches!(fmt, F_L | F_S | F_W | F_B),
        1 => false,
        7 => reg <= 1,
        _ => true,
    }
}

impl Cpu {
    /// Fetches the raw source operand of the instruction (op's EA).
    fn fetch_operand(&mut self, op: u16, size: u32) -> Res<[u32; 3]> {
        let mode = u32::from((op >> 3) & 7);
        let reg = u32::from(op & 7);
        let mut w = [0u32; 3];
        if mode == 0 {
            w[0] = self.r.d[reg as usize] & sz_mask(size);
            return Ok(w);
        }
        if mode == 7 && reg == 4 {
            // #imm: a byte is in the low half of a word
            if size <= 2 {
                w[0] = self.fetch16()? & sz_mask(size);
            } else {
                for i in 0..size / 4 {
                    w[i as usize] = self.fetch32()?;
                }
            }
            return Ok(w);
        }
        let mut e = self.ea_decode(mode, reg, size)?;
        let w = self.read_mem(e.addr, size, e.acc)?;
        self.ea_commit(&mut e);
        Ok(w)
    }

    /// The source converted to extended (exact, but for packed decimal).
    fn convert_in(&self, fmt: u32, w: &[u32; 3], flags: &mut u32) -> Fpx {
        let mut e = self.fpu_env();
        e.prec = 64;
        let v = match fmt {
            F_L => fpx::from_int(&mut e, w[0] as i32),
            F_W => fpx::from_int(&mut e, sext(w[0], SZ_W) as i32),
            F_B => fpx::from_int(&mut e, sext(w[0], SZ_B) as i32),
            F_S => fpx::from_single(w[0]),
            F_D => fpx::from_double(u64::from(w[0]) << 32 | u64::from(w[1])),
            F_X => fpx::make(
                w[0] >> 31 != 0,
                (w[0] >> 16) & 0x7FFF,
                u64::from(w[1]) << 32 | u64::from(w[2]),
            ),
            _ => fpx::from_packed(&mut e, w),
        };
        *flags |= e.flags;
        v
    }
}

// ---- Arithmetic --------------------------------------------------------------

/// What an opmode (command bits 6-0) is.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    /// not an instruction: line F
    None,
    Monadic,
    Dyadic,
    /// transcendental (fpx::trans)
    Trans(Func),
    SinCos,
    Cmp,
    Tst,
}

#[derive(Clone, Copy)]
struct OpInfo {
    kind: Kind,
    /// left to software by the 68040 (2: also 68060)
    soft040: u8,
}

fn opmode_info(m: u32) -> OpInfo {
    use Kind::*;
    let (kind, soft040) = match m {
        0x00 => (Monadic, 0), // FMOVE
        0x01 => (Monadic, 1), // FINT
        0x02 => (Trans(Func::Sinh), 2),
        0x03 => (Monadic, 1), // FINTRZ
        0x04 => (Monadic, 0), // FSQRT
        0x06 => (Trans(Func::Lognp1), 2),
        0x08 => (Trans(Func::Etoxm1), 2),
        0x09 => (Trans(Func::Tanh), 2),
        0x0A => (Trans(Func::Atan), 2),
        0x0C => (Trans(Func::Asin), 2),
        0x0D => (Trans(Func::Atanh), 2),
        0x0E => (Trans(Func::Sin), 2),
        0x0F => (Trans(Func::Tan), 2),
        0x10 => (Trans(Func::Etox), 2),
        0x11 => (Trans(Func::Twotox), 2),
        0x12 => (Trans(Func::Tentox), 2),
        0x14 => (Trans(Func::Logn), 2),
        0x15 => (Trans(Func::Log10), 2),
        0x16 => (Trans(Func::Log2), 2),
        0x18 => (Monadic, 0), // FABS
        0x19 => (Trans(Func::Cosh), 2),
        0x1A => (Monadic, 0), // FNEG
        0x1C => (Trans(Func::Acos), 2),
        0x1D => (Trans(Func::Cos), 2),
        0x1E => (Monadic, 2), // FGETEXP
        0x1F => (Monadic, 2), // FGETMAN
        0x20 => (Dyadic, 0),  // FDIV
        0x21 => (Dyadic, 2),  // FMOD
        0x22 => (Dyadic, 0),  // FADD
        0x23 => (Dyadic, 0),  // FMUL
        0x24 => (Dyadic, 2),  // FSGLDIV
        0x25 => (Dyadic, 2),  // FREM
        0x26 => (Dyadic, 2),  // FSCALE
        0x27 => (Dyadic, 2),  // FSGLMUL
        0x28 => (Dyadic, 0),  // FSUB
        0x30..=0x37 => (SinCos, 2),
        0x38 => (Cmp, 0),
        0x3A => (Tst, 0),
        _ => (None, 0),
    };
    OpInfo { kind, soft040 }
}

/// The 68040's single and double precision forms: the opmode they round
/// and their precision, or None.
fn sd_form(m: u32) -> Option<(u32, u32)> {
    const T: [(u32, u32, u32); 16] = [
        (0x40, 0x00, 24),
        (0x41, 0x04, 24),
        (0x44, 0x00, 53),
        (0x45, 0x04, 53),
        (0x58, 0x18, 24),
        (0x5A, 0x1A, 24),
        (0x5C, 0x18, 53),
        (0x5E, 0x1A, 53),
        (0x60, 0x20, 24),
        (0x62, 0x22, 24),
        (0x63, 0x23, 24),
        (0x64, 0x20, 53),
        (0x66, 0x22, 53),
        (0x67, 0x23, 53),
        (0x68, 0x28, 24),
        (0x6C, 0x28, 53),
    ];
    T.iter()
        .find(|t| t.0 == m)
        .map(|&(_, base, prec)| (base, prec))
}

fn monadic(e: &mut Env, m: u32, s: Fpx) -> Fpx {
    match m {
        0x00 => fpx::round(e, s),
        0x01 => {
            let mode = e.mode;
            fpx::int(e, s, mode)
        }
        0x03 => fpx::int(e, s, fpx::RZ),
        0x04 => fpx::sqrt(e, s),
        0x18 => fpx::abs(e, s),
        0x1A => fpx::neg(e, s),
        0x1E => fpx::getexp(e, s),
        _ => fpx::getman(e, s),
    }
}

impl Cpu {
    fn dyadic(&mut self, e: &mut Env, m: u32, d: Fpx, s: Fpx) -> Fpx {
        match m {
            0x20 => fpx::div(e, d, s),
            0x22 => fpx::add(e, d, s),
            0x23 => fpx::mul(e, d, s),
            0x24 => {
                e.prec = 24;
                fpx::div(e, d, s)
            }
            0x26 => fpx::scale(e, d, s),
            0x27 => {
                e.prec = 24;
                fpx::mul(e, d, s)
            }
            0x28 => fpx::sub(e, d, s),
            _ => {
                let (r, q) = fpx::rem(e, d, s, m == 0x25);
                self.r.fpsr = (self.r.fpsr & !FPSR_QUOT) | u32::from(q) << 16;
                r
            }
        }
    }

    /// The rest of an arithmetic instruction, once the source is known:
    /// opmode `m`, its base form and precision (the 68040's FSxxx/FDxxx).
    #[allow(clippy::too_many_arguments)]
    fn arith_result(
        &mut self,
        base: u32,
        prec: u32,
        info: OpInfo,
        m: u32,
        dst: usize,
        src: Fpx,
        mut flags: u32,
    ) {
        let mut e = self.fpu_env();
        if prec != 0 {
            e.prec = prec;
        }
        let res = match info.kind {
            Kind::Tst => {
                if fpx::is_snan(src) {
                    flags |= fpx::SNAN;
                }
                self.fpu_status(flags);
                self.set_fcc(fpx::cc(src));
                return;
            }
            Kind::Cmp => {
                let cc = fpx::cmp(&mut e, self.r.fp[dst], src);
                self.fpu_status(flags | e.flags);
                self.set_fcc(cc);
                return;
            }
            Kind::SinCos => {
                let mut e2 = e;
                let cosv = fpx::trans(&mut e2, Func::Cos, src);
                let res = fpx::trans(&mut e, Func::Sin, src);
                if self.fpu_status(flags | e.flags | e2.flags) {
                    self.r.fp[(m & 7) as usize] = cosv;
                    self.r.fp[dst] = res;
                    self.set_fcc(fpx::cc(res));
                }
                return;
            }
            Kind::Trans(f) => fpx::trans(&mut e, f, src),
            Kind::Monadic => monadic(&mut e, base, src),
            _ => {
                let d = self.r.fp[dst];
                self.dyadic(&mut e, base, d, src)
            }
        };
        if self.fpu_status(flags | e.flags) {
            self.r.fp[dst] = res;
            self.set_fcc(fpx::cc(res));
        }
    }
}

/// Opclasses 000 (FPm to FPn) and 010 (<ea> to FPn, FMOVECR).
fn op_arith(c: &mut Cpu, op: u16, cmd: u32) -> Res {
    let m = cmd & 0x7F;
    let dst = ((cmd >> 7) & 7) as usize;
    let spec = (cmd >> 10) & 7;
    let from_ea = cmd & 0x4000 != 0;
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);

    if from_ea && spec == 7 {
        // FMOVECR
        let mut e = c.fpu_env();
        let v = if c.internal_fpu() || op & 0x3F != 0 {
            None
        } else {
            fpx::constant(&mut e, m)
        };
        let Some(v) = v else {
            return c.fline(op);
        };
        if !c.fpu_begin(true)? {
            return Ok(());
        }
        c.r.fpiar = c.insn_pc;
        if c.fpu_status(e.flags) {
            c.r.fp[dst] = v;
            c.set_fcc(fpx::cc(v));
        }
        return Ok(());
    }

    let (base, prec) = if m >= 0x40 {
        match sd_form(m) {
            Some(bp) if c.internal_fpu() => bp,
            _ => return c.fline(op),
        }
    } else {
        (m, 0)
    };
    let info = opmode_info(base);
    if info.kind == Kind::None
        || (from_ea && !src_valid(mode, reg, spec))
        || (c.internal_fpu()
            && info.soft040 != 0
            && (c.model == Model::M68040 || info.soft040 == 2))
    {
        return c.fline(op);
    }
    if !c.fpu_begin(true)? {
        return Ok(());
    }
    c.r.fpiar = c.insn_pc;

    let mut flags = 0;
    let src = if from_ea {
        let w = c.fetch_operand(op, FMT_SIZE[spec as usize])?;
        c.convert_in(spec, &w, &mut flags)
    } else {
        c.r.fp[spec as usize]
    };

    c.arith_result(base, prec, info, m, dst, src, flags);
    Ok(())
}

/// Opclass 011: FMOVE FPm,<ea>.
fn op_fmove_out(c: &mut Cpu, op: u16, cmd: u32) -> Res {
    let fmt = (cmd >> 10) & 7;
    let src = ((cmd >> 7) & 7) as usize;
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    if !dst_valid(mode, reg, fmt) {
        return c.fline(op);
    }
    if !c.fpu_begin(true)? {
        return Ok(());
    }
    c.r.fpiar = c.insn_pc;
    let size = FMT_SIZE[fmt as usize];
    let ea = if mode != 0 {
        Some(c.ea_decode(mode, reg, size)?)
    } else {
        None
    };

    let (w, ok) = c.fmove_out_words(fmt, cmd, c.r.fp[src]);
    if !ok {
        return Ok(());
    }
    match ea {
        None => {
            c.set_dreg(reg as usize, w[0], size);
            Ok(())
        }
        Some(mut ea) => {
            c.write_mem(ea.addr, size, &w)?;
            c.ea_commit(&mut ea);
            Ok(())
        }
    }
}

impl Cpu {
    /// FMOVE FPm,<ea>: the value in format `fmt`; the status. Returns
    /// whether the destination may be written.
    fn fmove_out_words(&mut self, fmt: u32, cmd: u32, v: Fpx) -> ([u32; 3], bool) {
        let mut e = self.fpu_env();
        let mut w = [0u32; 3];
        match fmt {
            F_L => w[0] = fpx::to_int(&mut e, v, 4) as u32,
            F_W => w[0] = fpx::to_int(&mut e, v, 2) as u32 & 0xFFFF,
            F_B => w[0] = fpx::to_int(&mut e, v, 1) as u32 & 0xFF,
            F_S => w[0] = fpx::to_single(&mut e, v),
            F_D => {
                let d = fpx::to_double(&mut e, v);
                w[0] = (d >> 32) as u32;
                w[1] = d as u32;
            }
            F_X => {
                let x = fpx::to_extended(&mut e, v);
                w[0] = u32::from(x.sign_exponent) << 16;
                w[1] = (x.mantissa >> 32) as u32;
                w[2] = x.mantissa as u32;
            }
            _ => {
                // the k-factor: 7 bits with sign, static or in Dn
                let kreg = if fmt == F_P {
                    cmd
                } else {
                    self.r.d[((cmd >> 4) & 7) as usize]
                };
                let mut k = (kreg & 0x7F) as i32;
                if k & 0x40 != 0 {
                    k -= 128;
                }
                w = fpx::to_packed(&mut e, v, k);
            }
        }
        (w, self.fpu_status(e.flags))
    }
}

// ---- FMOVEM ------------------------------------------------------------------

/// Opclasses 100 and 101: the control registers (bit 12 FPCR, 11 FPSR, 10
/// FPIAR) from or to <ea>, in that order in memory.
fn op_fmovem_ctrl(c: &mut Cpu, op: u16, cmd: u32) -> Res {
    let to_ea = cmd & 0x2000 != 0;
    let list = (cmd >> 10) & 7;
    let n = list.count_ones();
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    let r = reg as usize;
    let mut ok = n != 0;
    if mode == 0 {
        ok = ok && n == 1;
    } else if mode == 1 {
        ok = ok && list == 1;
    } else if mode == 7 && reg >= 2 {
        ok = ok && !to_ea && (reg <= 3 || reg == 4);
    }
    if mode == 7 && reg > 4 {
        ok = false;
    }
    if !ok {
        return c.fline(op);
    }
    c.fpu_begin(false)?;
    const MASKS: [u32; 3] = [FPCR_MASK, FPSR_MASK, 0xFFFF_FFFF];

    fn creg(c: &mut Cpu, i: usize) -> &mut u32 {
        match i {
            0 => &mut c.r.fpcr,
            1 => &mut c.r.fpsr,
            _ => &mut c.r.fpiar,
        }
    }

    if mode <= 1 {
        for (i, &mask) in MASKS.iter().enumerate() {
            if list & (4 >> i) == 0 {
                continue;
            }
            if to_ea {
                let v = *creg(c, i);
                if mode == 0 {
                    c.r.d[r] = v;
                } else {
                    c.r.a[r] = v;
                }
            } else {
                let v = if mode == 0 { c.r.d[r] } else { c.r.a[r] };
                *creg(c, i) = v & mask;
            }
        }
        return Ok(());
    }
    let imm = mode == 7 && reg == 4;
    let mut addr = if mode == 4 {
        let a = c.r.a[r].wrapping_sub(4 * n);
        c.r.a[r] = a;
        a
    } else if mode == 3 {
        let a = c.r.a[r];
        c.r.a[r] = a.wrapping_add(4 * n);
        a
    } else if imm {
        0
    } else {
        c.ea_decode(mode, reg, SZ_L)?.addr
    };
    let acc = if mode == 7 && (reg == 2 || reg == 3) {
        ACC_PROG
    } else {
        ACC_DATA
    };
    for (i, &mask) in MASKS.iter().enumerate() {
        if list & (4 >> i) == 0 {
            continue;
        }
        if to_ea {
            let v = *creg(c, i);
            c.write32(addr, v)?;
        } else {
            let v = if imm {
                c.fetch32()?
            } else {
                c.read32(addr, acc)?
            };
            *creg(c, i) = v & mask;
        }
        addr = addr.wrapping_add(4);
    }
    Ok(())
}

/// Opclasses 110 and 111: FMOVEM.X of data registers. Bit 11 of the
/// command: list in Dn; bit 12: postincrement or control mode (list bit 7
/// is FP0) rather than predecrement (bit 0 is FP0). FP0 is always at the
/// lowest address.
fn op_fmovem_x(c: &mut Cpu, op: u16, cmd: u32) -> Res {
    let to_ea = cmd & 0x2000 != 0;
    let postinc = cmd & 0x1000 != 0;
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    let r = reg as usize;
    let ok = if to_ea {
        if postinc {
            mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1)
        } else {
            mode == 4
        }
    } else {
        postinc && (mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3))
    };
    if !ok {
        return c.fline(op);
    }
    if !c.fpu_begin(true)? {
        return Ok(());
    }
    let list = if cmd & 0x0800 != 0 {
        c.r.d[((cmd >> 4) & 7) as usize] & 0xFF
    } else {
        cmd & 0xFF
    };
    let n = list.count_ones();
    let mut acc = ACC_DATA;
    let mut addr = if mode == 4 {
        c.r.a[r].wrapping_sub(12 * n)
    } else if mode == 3 {
        c.r.a[r]
    } else {
        let e = c.ea_decode(mode, reg, SZ_L)?;
        acc = e.acc;
        e.addr
    };
    let a0 = addr;
    for i in 0..8 {
        let bit = if postinc { 0x80 >> i } else { 1 << i };
        if list & bit == 0 {
            continue;
        }
        if to_ea {
            let f = c.r.fp[i];
            let w = [
                u32::from(f.sign_exponent) << 16,
                (f.mantissa >> 32) as u32,
                f.mantissa as u32,
            ];
            c.write_mem(addr, 12, &w)?;
        } else {
            let w = c.read_mem(addr, 12, acc)?;
            c.r.fp[i].sign_exponent = (w[0] >> 16) as u16;
            c.r.fp[i].mantissa = u64::from(w[1]) << 32 | u64::from(w[2]);
        }
        addr = addr.wrapping_add(12);
    }
    if mode == 4 {
        c.r.a[r] = a0;
    } else if mode == 3 {
        c.r.a[r] = addr;
    }
    Ok(())
}

fn op_fgen(c: &mut Cpu, op: u16) -> Res {
    let cmd = c.fetch16()?;
    match cmd >> 13 {
        0 | 2 => op_arith(c, op, cmd),
        3 => op_fmove_out(c, op, cmd),
        4 | 5 => op_fmovem_ctrl(c, op, cmd),
        6 | 7 => op_fmovem_x(c, op, cmd),
        _ => c.fline(op),
    }
}

// ---- Conditions --------------------------------------------------------------

impl Cpu {
    /// Evaluates a conditional predicate (MC68881UM 4.4). Predicates 0x10 to
    /// 0x1F set BSUN when NAN is set, and take its exception at once if
    /// enabled: then None.
    fn fp_cond(&mut self, pred: u32) -> Res<Option<bool>> {
        let cc = self.r.fpsr;
        let n = cc & fpx::CC_N != 0;
        let z = cc & fpx::CC_Z != 0;
        let nan = cc & fpx::CC_NAN != 0;
        if pred & 0x10 != 0 && nan {
            self.r.fpsr |= fpx::BSUN | 0x80;
            if self.r.fpcr & fpx::BSUN != 0 {
                self.exception_group1(VEC_FP_BSUN)?;
                return Ok(None);
            }
        }
        Ok(Some(match pred & 0xF {
            0x0 => false,            // F, SF
            0x1 => z,                // EQ, SEQ
            0x2 => !(nan || z || n), // OGT, GT
            0x3 => z || !(nan || n), // OGE, GE
            0x4 => n && !(nan || z), // OLT, LT
            0x5 => z || (n && !nan), // OLE, LE
            0x6 => !(nan || z),      // OGL, GL
            0x7 => !nan,             // OR, GLE
            0x8 => nan,              // UN, NGLE
            0x9 => nan || z,         // UEQ, NGL
            0xA => nan || !(n || z), // UGT, NLE
            0xB => nan || z || !n,   // UGE, NLT
            0xC => nan || (n && !z), // ULT, NGE
            0xD => nan || z || n,    // ULE, NGT
            0xE => !z,               // NE, SNE
            _ => true,               // T, ST
        }))
    }
}

/// FBcc (F280 + predicate: 16-bit displacement, F2C0: 32-bit).
fn op_fbcc(c: &mut Cpu, op: u16) -> Res {
    if op & 0x20 != 0 {
        return c.fline(op);
    }
    if !c.fpu_begin(true)? {
        return Ok(());
    }
    c.r.fpiar = c.insn_pc;
    let base = c.r.pc;
    let disp = if op & 0x40 != 0 {
        c.fetch32()?
    } else {
        sext(c.fetch16()?, SZ_W)
    };
    if c.fp_cond(u32::from(op & 0x3F))? == Some(true) {
        return c.jump(base.wrapping_add(disp), c.r.pc);
    }
    Ok(())
}

/// F240 + ea: FScc; F248 + n: FDBcc; F27A/F27B/F27C: FTRAPcc. The
/// predicate is in the extension word.
fn op_fcc_misc(c: &mut Cpu, op: u16) -> Res {
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    if c.model == Model::M68060 || (mode == 7 && reg > 4) {
        return c.fline(op);
    }
    let ext = c.fetch16()?;
    if ext & 0xFFE0 != 0 {
        return c.fline(op);
    }
    if !c.fpu_begin(true)? {
        return Ok(());
    }
    c.r.fpiar = c.insn_pc;
    if mode == 1 {
        // FDBcc
        let base = c.r.pc;
        let disp = sext(c.fetch16()?, SZ_W);
        match c.fp_cond(ext)? {
            None | Some(true) => return Ok(()),
            Some(false) => {}
        }
        let r = reg as usize;
        let n = c.r.d[r].wrapping_sub(1) & 0xFFFF;
        c.set_dreg(r, n, SZ_W);
        if n != 0xFFFF {
            return c.jump(base.wrapping_add(disp), c.r.pc);
        }
        return Ok(());
    }
    if mode == 7 && reg >= 2 {
        // FTRAPcc
        if reg == 2 {
            c.fetch16()?;
        } else if reg == 3 {
            c.fetch32()?;
        }
        if c.fp_cond(ext)? == Some(true) {
            return c.exception(VEC_TRAPV, c.r.pc);
        }
        return Ok(());
    }
    // FScc
    let mut e = c.ea_decode(mode, reg, SZ_B)?;
    match c.fp_cond(ext)? {
        None => Ok(()),
        Some(t) => c.ea_write(&mut e, if t { 0xFF } else { 0 }),
    }
}

// ---- FSAVE and FRESTORE ------------------------------------------------------

impl Cpu {
    /// The idle frame (MC68881UM 6.4, MC68040UM 9.6, MC68060UM 6.5): its
    /// longs and its total size. The internal words are 0, but one byte
    /// keeps a pending exception's vector.
    fn idle_frame(&self) -> ([u32; 16], u32) {
        let mut w = [0u32; 16];
        let size = match self.fpu {
            Fpu::M68881 => {
                w[0] = 0x1F18_0000;
                w[6] = self.fpu_pending;
                28
            }
            Fpu::M68882 => {
                w[0] = 0x1F38_0000;
                w[14] = self.fpu_pending;
                60
            }
            _ => {
                if self.model == Model::M68060 {
                    w[0] = 0x0000_6000;
                    w[2] = self.fpu_pending;
                    12
                } else {
                    w[0] = 0x4100_0000 | self.fpu_pending;
                    4
                }
            }
        };
        (w, size)
    }
}

fn op_fsave(c: &mut Cpu, op: u16) -> Res {
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    if !(mode == 2 || mode == 4 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1)) {
        return c.fline(op);
    }
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let (w, size) = if c.fpu_null {
        ([0u32; 16], if c.model == Model::M68060 { 12 } else { 4 })
    } else {
        c.idle_frame()
    };
    let r = reg as usize;
    let addr = if mode == 4 {
        c.r.a[r].wrapping_sub(size)
    } else {
        c.ea_decode(mode, reg, SZ_L)?.addr
    };
    for i in 0..size / 4 {
        c.write32(addr.wrapping_add(4 * i), w[i as usize])?;
    }
    if mode == 4 {
        c.r.a[r] = addr;
    }
    c.fpu_pending = 0;
    Ok(())
}

fn op_frestore(c: &mut Cpu, op: u16) -> Res {
    let mode = u32::from((op >> 3) & 7);
    let reg = u32::from(op & 7);
    if !(mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3)) {
        return c.fline(op);
    }
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    let r = reg as usize;
    let mut acc = ACC_DATA;
    let addr = if mode == 3 {
        c.r.a[r]
    } else {
        let e = c.ea_decode(mode, reg, SZ_L)?;
        acc = e.acc;
        e.addr
    };
    let w0 = c.read32(addr, acc)?;
    let mut size;
    let mut pending = 0;
    let null;
    let valid;
    if c.model == Model::M68060 {
        let ty = (w0 >> 8) & 0xFF;
        size = 12;
        null = ty == 0;
        valid = null || ty == 0x60 || ty == 0xE0;
        if valid && !null {
            pending = c.read32(addr.wrapping_add(8), acc)? & 0xFF;
        }
    } else {
        let ver = w0 >> 24;
        let len = (w0 >> 16) & 0xFF;
        size = 4 + len;
        null = ver == 0;
        match c.fpu {
            Fpu::M68881 => {
                valid = ver == 0x1F && (len == 0x18 || len == 0xB4);
                if valid && len == 0x18 {
                    pending = c.read32(addr.wrapping_add(24), acc)? & 0xFF;
                }
            }
            Fpu::M68882 => {
                valid = ver == 0x1F && (len == 0x38 || len == 0xD4);
                if valid && len == 0x38 {
                    pending = c.read32(addr.wrapping_add(56), acc)? & 0xFF;
                }
            }
            _ => {
                valid = ver == 0x41 && (len == 0 || len == 0x30 || len == 0x60);
                if valid && len == 0 {
                    pending = w0 & 0xFF;
                }
            }
        }
        if null {
            size = 4;
        }
    }
    if !null && !valid {
        return c.exception_group1(VEC_FORMAT_ERROR);
    }
    if null {
        c.fpu_reset();
    } else {
        c.fpu_null = false;
        c.fpu_pending = if (VEC_FP_BSUN..=VEC_FP_SNAN).contains(&pending) {
            pending
        } else {
            0
        };
    }
    if mode == 3 {
        c.r.a[r] = addr.wrapping_add(size);
    }
    Ok(())
}

// ---- The table -----------------------------------------------------------------

pub fn fill(tb: &mut OpTable) {
    if tb.fpu == Fpu::None {
        return;
    }
    for ea in 0..64u16 {
        tb.set(0xF200 | ea, op_fgen);
        tb.set(0xF240 | ea, op_fcc_misc);
        tb.set(0xF280 | ea, op_fbcc);
        tb.set(0xF2C0 | ea, op_fbcc);
        tb.set(0xF300 | ea, op_fsave);
        tb.set(0xF340 | ea, op_frestore);
    }
}
