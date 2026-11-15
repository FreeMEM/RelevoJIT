// SPDX-License-Identifier: MPL-2.0
//! Integer instructions of the 68020 and later (M68000PRM chapter 4,
//! MC68020UM): MULU.L, MULS.L, DIVU.L, DIVS.L, DIVUL.L, DIVSL.L, EXTB.L,
//! CHK.L, CHK2, CMP2, CAS, CAS2, TRAPcc, PACK, UNPK, LINK.L, and CMPI and
//! TST with the operands the 68020 allows them; and MOVE16, CINV and CPUSH
//! of the 68040. Bcc.L, BRA.L and BSR.L are in control.rs; the 68020
//! addressing modes in ea.rs; bit fields in bitfield.rs.
//!
//! The 68060 traps MULx.L and DIVx.L with 64 bits, CHK2, CMP2 and CAS2 to
//! software ("unimplemented integer instruction"); here it runs them as the
//! 68040 does, which gives what that software would.
//!
//! Cycles: there is no exact reference for these models. Bus accesses count
//! themselves as on the 68000; the handlers add rough internal times.

use super::{
    CCR_C, CCR_N, CCR_V, CCR_Z, EA_CONTROL, EA_DATA, EA_MEM_ALT, EAB_AREG, EAB_IMM, EAB_PCD8,
    EAB_PCD16, OpTable, SZ_B, SZ_L, SZ_W, cond_true, reg0, reg9, sext, size_field, sz_mask, sz_msb,
};
use crate::cpu::{ACC_DATA, Cpu, Model, Res, VEC_CHK, VEC_TRAPV, VEC_ZERO_DIVIDE};

impl Cpu {
    /// Register 0-15 of an extension word's bits 15-12: D0-D7, A0-A7.
    fn reg_value(&self, n: u32) -> u32 {
        let n = (n & 15) as usize;
        if n < 8 { self.r.d[n] } else { self.r.a[n - 8] }
    }

    /// The flags of CMP: d - s, X unchanged. Returns the Z flag.
    fn cmp_flags(&mut self, d: u32, s: u32, size: u32) -> bool {
        let m = sz_mask(size);
        let msb = sz_msb(size);
        let d = d & m;
        let s = s & m;
        let r = d.wrapping_sub(s) & m;
        let mut f = 0;
        if r & msb != 0 {
            f |= CCR_N;
        }
        if r == 0 {
            f |= CCR_Z;
        }
        if (s ^ d) & (r ^ d) & msb != 0 {
            f |= CCR_V;
        }
        if ((s & !d) | ((s | !d) & r)) & msb != 0 {
            f |= CCR_C;
        }
        self.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, f);
        r == 0
    }

    /// CHK.W and CHK.L on the 68020 and later. The manuals define only N
    /// (set if Dn < 0, clear if Dn > bound). As FS-UAE, the bench's oracle:
    /// Dn > bound is checked first (so N is clear when both hold), and Z, V
    /// and C do not change, nor does N when there is no trap.
    pub fn chk_020(&mut self, v: i32, bound: i32) -> Res {
        if v > bound {
            self.set_ccr_bits(CCR_N, 0);
            self.exception(VEC_CHK, self.r.pc)
        } else if v < 0 {
            self.set_ccr_bits(CCR_N, CCR_N);
            self.exception(VEC_CHK, self.r.pc)
        } else {
            Ok(())
        }
    }
}

// ---- MULU.L, MULS.L ------------------------------------------------------

/// MULx.L <ea>,Dl (32 x 32 -> 32, V on overflow) or <ea>,Dh:Dl (-> 64).
/// Extension word: Dl in bits 14-12, signed in bit 11, 64-bit result in
/// bit 10, Dh in bits 2-0. N and Z come from the whole 64-bit product, also
/// for a 32-bit result that overflows; with Dh = Dl (undefined) the low half
/// stays. Both as FS-UAE, the bench's oracle.
fn op_mul_l(c: &mut Cpu, op: u16) -> Res {
    let ext = c.fetch16()?;
    let mut e = c.ea_decode_op(op, SZ_L)?;
    let s = c.ea_read(&mut e)?;
    let dl = ((ext >> 12) & 7) as usize;
    let dh = (ext & 7) as usize;
    let sign = ext & 0x0800 != 0;
    let p: u64 = if sign {
        (i64::from(c.r.d[dl] as i32) * i64::from(s as i32)) as u64
    } else {
        u64::from(c.r.d[dl]) * u64::from(s)
    };

    let mut f = 0;
    let lo = p as u32;
    let hi = (p >> 32) as u32;
    if p == 0 {
        f |= CCR_Z;
    }
    if hi & 0x8000_0000 != 0 {
        f |= CCR_N;
    }
    if ext & 0x0400 != 0 {
        c.r.d[dh] = hi;
        c.r.d[dl] = lo;
    } else {
        c.r.d[dl] = lo;
        // the high half is not just the extension of the low one
        let ext_hi = if sign && lo & 0x8000_0000 != 0 {
            0xFFFF_FFFF
        } else {
            0
        };
        if hi != ext_hi {
            f |= CCR_V;
        }
    }
    c.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, f);
    c.cycles(40);
    Ok(())
}

// ---- DIVU.L, DIVS.L, DIVUL.L, DIVSL.L --------------------------------------

/// Extension word: Dq in bits 14-12, signed in bit 11, 64-bit dividend in
/// bit 10, Dr in bits 2-0.
///
/// ```text
///   bit 10 clear, Dr = Dq:  DIVx.L <ea>,Dq       32/32 -> 32q
///   bit 10 clear, Dr != Dq: DIVxL.L <ea>,Dr:Dq   32/32 -> 32r:32q
///   bit 10 set:             DIVx.L <ea>,Dr:Dq    64/32 -> 32r:32q
/// ```
///
/// The dividend is Dq (Dr:Dq for 64 bits). The remainder has the sign of
/// the dividend. When Dr = Dq only the quotient is kept. On overflow the
/// registers do not change; V is set, C cleared, N set and Z cleared (the
/// manuals leave N and Z undefined; FS-UAE, the bench's oracle, sets N too,
/// and Z in some cases only, by a rule not found yet). Division by zero
/// leaves the flags as they were (as FS-UAE).
fn op_div_l(c: &mut Cpu, op: u16) -> Res {
    let ext = c.fetch16()?;
    let mut e = c.ea_decode_op(op, SZ_L)?;
    let s = c.ea_read(&mut e)?;
    let dq = ((ext >> 12) & 7) as usize;
    let dr = (ext & 7) as usize;
    let sign = ext & 0x0800 != 0;
    let wide = ext & 0x0400 != 0;

    if s == 0 {
        c.cycles(4);
        return c.exception(VEC_ZERO_DIVIDE, c.r.pc);
    }
    c.cycles(70);

    let mut n: u64 = if wide {
        u64::from(c.r.d[dr]) << 32 | u64::from(c.r.d[dq])
    } else {
        u64::from(c.r.d[dq])
    };
    let (q, r);
    if sign {
        if !wide {
            n = i64::from(n as u32 as i32) as u64;
        }
        let nneg = n >> 63 != 0;
        let sneg = s >> 31 != 0;
        // magnitudes: 0 - n works for the most negative dividend too
        let an = if nneg { n.wrapping_neg() } else { n };
        let as_ = if sneg {
            (i64::from(s as i32) as u64).wrapping_neg()
        } else {
            u64::from(s)
        };
        let aq = an / as_;
        let ar = an % as_;
        let qneg = nneg != sneg;
        if aq > if qneg { 0x8000_0000 } else { 0x7FFF_FFFF } {
            c.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
            return Ok(());
        }
        q = (if qneg { aq.wrapping_neg() } else { aq }) as u32;
        r = (if nneg { ar.wrapping_neg() } else { ar }) as u32;
    } else {
        let uq = n / u64::from(s);
        if uq >> 32 != 0 {
            c.set_ccr_bits(CCR_N | CCR_Z | CCR_V | CCR_C, CCR_N | CCR_V);
            return Ok(());
        }
        q = uq as u32;
        r = (n % u64::from(s)) as u32;
    }
    c.r.d[dr] = r;
    c.r.d[dq] = q;
    c.set_nz(q, SZ_L);
    Ok(())
}

// ---- EXTB.L, LINK.L ----------------------------------------------------------

fn op_extb_l(c: &mut Cpu, op: u16) -> Res {
    let v = sext(c.r.d[reg0(op)], SZ_B);
    c.r.d[reg0(op)] = v;
    c.set_nz(v, SZ_L);
    Ok(())
}

/// LINK.L An,#d32: as LINK.W (A7 stores its value before the push).
fn op_link_l(c: &mut Cpu, op: u16) -> Res {
    let n = reg0(op);
    let d = c.fetch32()?;
    c.push32(c.r.a[n])?;
    c.r.a[n] = c.r.a[7];
    c.r.a[7] = c.r.a[7].wrapping_add(d);
    c.cycles(2);
    Ok(())
}

// ---- CHK.L, CHK2, CMP2 -------------------------------------------------------

/// CHK.L <ea>,Dn: as CHK.W, on 32 bits.
fn op_chk_l(c: &mut Cpu, op: u16) -> Res {
    let mut e = c.ea_decode_op(op, SZ_L)?;
    let bound = c.ea_read(&mut e)? as i32;
    c.cycles(4);
    c.chk_020(c.r.d[reg9(op)] as i32, bound)
}

/// CHK2/CMP2 <ea>,Rn: the lower bound at <ea>, the upper one after it.
/// Extension word: register in bits 15-12, CHK2 in bit 11.
///
/// Bounds and register are taken sign-extended to 32 bits (for a data
/// register only its low byte or word counts; for an address register the
/// bounds are extended and the whole register compared, MC68020UM). Rn is
/// in bounds if Rn - lower <= upper - lower, unsigned: that is lower <= Rn
/// <= upper whether the program means them signed or unsigned, as long as
/// lower is the smaller one in its meaning. Z: Rn equals a bound. C: Rn is
/// out of bounds. N and V are left as they were (undefined). CHK2 traps
/// (vector 6) when C is set.
fn op_chk2(c: &mut Cpu, op: u16) -> Res {
    const SIZES: [u32; 4] = [SZ_B, SZ_W, SZ_L, SZ_L];
    let size = SIZES[usize::from((op >> 9) & 3)];
    let ext = c.fetch16()?;
    let e = c.ea_decode_op(op, size)?;
    let lo = sext(c.read(e.addr, size, e.acc)?, size);
    let hi = sext(c.read(e.addr.wrapping_add(size), size, e.acc)?, size);
    let rn = (ext >> 12) & 15;
    let mut v = c.reg_value(rn);
    if rn < 8 {
        v = sext(v, size);
    }
    let mut f = 0;
    if v == lo || v == hi {
        f |= CCR_Z;
    } else if v.wrapping_sub(lo) > hi.wrapping_sub(lo) {
        f |= CCR_C;
    }
    c.set_ccr_bits(CCR_Z | CCR_C, f);
    c.cycles(8);
    if ext & 0x0800 != 0 && f & CCR_C != 0 {
        return c.exception(VEC_CHK, c.r.pc);
    }
    Ok(())
}

// ---- CAS, CAS2 ---------------------------------------------------------------

fn cas_size(op: u16) -> u32 {
    const SIZES: [u32; 4] = [0, SZ_B, SZ_W, SZ_L];
    SIZES[usize::from((op >> 9) & 3)]
}

/// CAS Dc,Du,<ea>: compare <ea> with Dc; if equal, Du goes to <ea>,
/// otherwise <ea> goes to Dc. Extension word: Du in bits 8-6, Dc in 2-0.
fn op_cas(c: &mut Cpu, op: u16) -> Res {
    let size = cas_size(op);
    let ext = c.fetch16()?;
    let mut e = c.ea_decode_op(op, size)?;
    let m = c.ea_read(&mut e)?;
    let dc = (ext & 7) as usize;
    let du = ((ext >> 6) & 7) as usize;
    c.cycles(4);
    if c.cmp_flags(m, c.r.d[dc], size) {
        c.ea_write(&mut e, c.r.d[du])
    } else {
        c.set_dreg(dc, m, size);
        Ok(())
    }
}

/// CAS2 Dc1:Dc2,Du1:Du2,(Rn1):(Rn2), word or long. The flags are those of
/// the first comparison, or of the second when the first is equal. Both
/// equal: Du1 to (Rn1), then Du2 to (Rn2). Otherwise both memory operands
/// go to Dc1 and Dc2; with Dc1 = Dc2 the first operand is kept.
fn op_cas2(c: &mut Cpu, op: u16) -> Res {
    let size = cas_size(op);
    let e1 = c.fetch16()?;
    let e2 = c.fetch16()?;
    let a1 = c.reg_value(e1 >> 12);
    let a2 = c.reg_value(e2 >> 12);
    let dc1 = (e1 & 7) as usize;
    let du1 = ((e1 >> 6) & 7) as usize;
    let dc2 = (e2 & 7) as usize;
    let du2 = ((e2 >> 6) & 7) as usize;
    let m1 = c.read(a1, size, ACC_DATA)?;
    let m2 = c.read(a2, size, ACC_DATA)?;
    c.cycles(8);
    if c.cmp_flags(m1, c.r.d[dc1], size) && c.cmp_flags(m2, c.r.d[dc2], size) {
        c.write(a1, c.r.d[du1], size)?;
        c.write(a2, c.r.d[du2], size)
    } else {
        c.set_dreg(dc2, m2, size);
        c.set_dreg(dc1, m1, size);
        Ok(())
    }
}

// ---- TRAPcc ------------------------------------------------------------------

/// TRAPcc, TRAPcc.W #d16, TRAPcc.L #d32: the operand is only for the
/// handler to read. If the condition holds, the TRAPV exception (vector 7)
/// with the address of the next instruction.
fn op_trapcc(c: &mut Cpu, op: u16) -> Res {
    if op & 7 == 2 {
        c.fetch16()?;
    } else if op & 7 == 3 {
        c.fetch32()?;
    }
    if cond_true(c.r.sr, u32::from(op >> 8)) {
        return c.exception(VEC_TRAPV, c.r.pc);
    }
    Ok(())
}

// ---- PACK, UNPK --------------------------------------------------------------

impl Cpu {
    /// -(An) for one byte: A7 stays even.
    fn predec_byte(&mut self, n: usize) -> u32 {
        self.r.a[n] = self.r.a[n].wrapping_sub(if n == 7 { 2 } else { 1 });
        self.r.a[n]
    }
}

/// PACK Dx,Dy,#adj or PACK -(Ax),-(Ay),#adj: the word (two unpacked BCD
/// digits in bits 11-8 and 3-0) plus the adjustment, packed into a byte.
/// From memory the word is two bytes, the low one read first. Flags
/// unchanged.
fn op_pack(c: &mut Cpu, op: u16) -> Res {
    let adj = c.fetch16()?;
    let x = reg0(op);
    let y = reg9(op);
    let mut v = if op & 8 != 0 {
        let a = c.predec_byte(x);
        let lo = c.read8(a, ACC_DATA)?;
        let a = c.predec_byte(x);
        let hi = c.read8(a, ACC_DATA)?;
        hi << 8 | lo
    } else {
        c.r.d[x]
    };
    v = v.wrapping_add(adj) & 0xFFFF;
    let b = ((v >> 4) & 0xF0) | (v & 0x0F);
    if op & 8 != 0 {
        let a = c.predec_byte(y);
        c.write8(a, b)?;
    } else {
        c.set_dreg(y, b, SZ_B);
    }
    c.cycles(4);
    Ok(())
}

/// UNPK Dx,Dy,#adj or UNPK -(Ax),-(Ay),#adj: the two digits of a byte
/// spread to bits 11-8 and 3-0, plus the adjustment. To memory the low
/// byte goes first. Flags unchanged.
fn op_unpk(c: &mut Cpu, op: u16) -> Res {
    let adj = c.fetch16()?;
    let x = reg0(op);
    let y = reg9(op);
    let b = if op & 8 != 0 {
        let a = c.predec_byte(x);
        c.read8(a, ACC_DATA)?
    } else {
        c.r.d[x] & 0xFF
    };
    let v = ((((b & 0xF0) << 4) | (b & 0x0F)).wrapping_add(adj)) & 0xFFFF;
    if op & 8 != 0 {
        let a = c.predec_byte(y);
        c.write8(a, v & 0xFF)?;
        let a = c.predec_byte(y);
        c.write8(a, v >> 8)?;
    } else {
        c.set_dreg(y, v, SZ_W);
    }
    c.cycles(4);
    Ok(())
}

// ---- CMPI and TST with the 68020's operands --------------------------------

/// CMPI #imm,<ea> with a PC-relative <ea>.
fn op_cmpi_pc(c: &mut Cpu, op: u16) -> Res {
    let size = size_field(op);
    let s = if size == SZ_L {
        c.fetch32()?
    } else {
        c.fetch16()? & sz_mask(size)
    };
    let mut e = c.ea_decode_op(op, size)?;
    let d = c.ea_read(&mut e)?;
    c.cmp_flags(d, s, size);
    Ok(())
}

/// TST <ea> with An (word and long), PC-relative or immediate <ea>.
fn op_tst_020(c: &mut Cpu, op: u16) -> Res {
    let size = size_field(op);
    let mut e = c.ea_decode_op(op, size)?;
    let v = c.ea_read(&mut e)?;
    c.set_nz(v, size);
    Ok(())
}

// ---- MOVE16, CINV, CPUSH (68040 and 68060) ---------------------------------

/// Copies the 16-byte line holding `src` to the one holding `dst`.
fn move16_line(c: &mut Cpu, src: u32, dst: u32) -> Res {
    let src = src & !15;
    let dst = dst & !15;
    let mut line = [0u32; 4];
    for (i, w) in line.iter_mut().enumerate() {
        *w = c.read32(src.wrapping_add(4 * i as u32), ACC_DATA)?;
    }
    for (i, &w) in line.iter().enumerate() {
        c.write32(dst.wrapping_add(4 * i as u32), w)?;
    }
    c.cycles(4);
    Ok(())
}

/// MOVE16 (Ax)+,(Ay)+: Ay in bits 14-12 of the extension word. Both
/// registers go up by 16 (the same register twice, if Ax = Ay).
fn op_move16_postinc(c: &mut Cpu, op: u16) -> Res {
    let ext = c.fetch16()?;
    let ax = reg0(op);
    let ay = ((ext >> 12) & 7) as usize;
    move16_line(c, c.r.a[ax], c.r.a[ay])?;
    c.r.a[ax] = c.r.a[ax].wrapping_add(16);
    c.r.a[ay] = c.r.a[ay].wrapping_add(16);
    Ok(())
}

/// MOVE16 with an absolute address; bits 4-3: 00 (Ay)+,(xxx).L;
/// 01 (xxx).L,(Ay)+; 10 (Ay),(xxx).L; 11 (xxx).L,(Ay).
fn op_move16_abs(c: &mut Cpu, op: u16) -> Res {
    let abs = c.fetch32()?;
    let ay = reg0(op);
    let mode = (op >> 3) & 3;
    if mode & 1 != 0 {
        move16_line(c, abs, c.r.a[ay])?;
    } else {
        move16_line(c, c.r.a[ay], abs)?;
    }
    if mode & 2 == 0 {
        c.r.a[ay] = c.r.a[ay].wrapping_add(16);
    }
    Ok(())
}

/// CINV and CPUSH (privileged): 1111 0100 cc p ss rrr, caches in bits 7-6
/// (bit 6 data, bit 7 instruction), CPUSH in bit 5, scope in bits 4-3 (01
/// line, 10 page, 11 all; An holds the physical address). The data cache
/// needs nothing (memory is always up to date); code in the instruction
/// cache is dropped from the JIT.
fn op_cache(c: &mut Cpu, op: u16) -> Res {
    if !c.supervisor_or_trap()? {
        return Ok(());
    }
    c.cycles(4);
    if op & 0x0080 == 0 {
        return Ok(());
    }
    let addr = c.r.a[reg0(op)];
    match (op >> 3) & 3 {
        1 => c.invalidate_line(addr),
        2 => c.invalidate_page(addr),
        _ => c.invalidate_all(),
    }
    Ok(())
}

// ---- Table -------------------------------------------------------------------

pub fn fill(tb: &mut OpTable) {
    if tb.model < Model::M68020 {
        return;
    }

    tb.fill(0x4C00, 0x003F, EA_DATA, op_mul_l);
    tb.fill(0x4C40, 0x003F, EA_DATA, op_div_l);
    tb.fill(0x49C0, 0x0007, 0, op_extb_l);
    tb.fill(0x4808, 0x0007, 0, op_link_l);
    tb.fill(0x4100, 0x0E3F, EA_DATA, op_chk_l);
    for s in 0..3u16 {
        tb.fill(0x00C0 | s << 9, 0x003F, EA_CONTROL, op_chk2);
        tb.fill(0x0AC0 + (s << 9), 0x003F, EA_MEM_ALT, op_cas);
    }
    tb.set(0x0CFC, op_cas2);
    tb.set(0x0EFC, op_cas2);
    for cc in 0..16u16 {
        for mode in 2..=4u16 {
            tb.set(0x50F8 | cc << 8 | mode, op_trapcc);
        }
    }
    tb.fill(0x8140, 0x0E0F, 0, op_pack);
    tb.fill(0x8180, 0x0E0F, 0, op_unpk);

    for s in 0..3u16 {
        tb.fill(0x0C00 | s << 6, 0x003F, EAB_PCD16 | EAB_PCD8, op_cmpi_pc);
        let tst = EAB_PCD16 | EAB_PCD8 | EAB_IMM | if s != 0 { EAB_AREG } else { 0 };
        tb.fill(0x4A00 | s << 6, 0x003F, tst, op_tst_020);
    }

    super::bitfield::fill(tb);

    if tb.model >= Model::M68040 {
        tb.fill(0xF620, 0x0007, 0, op_move16_postinc);
        tb.fill(0xF600, 0x001F, 0, op_move16_abs);
        // scope 00 is not an instruction: it stays line F
        for scope in 1..4u16 {
            tb.fill(0xF400 | scope << 3, 0x00E7, 0, op_cache);
        }
    }
}
