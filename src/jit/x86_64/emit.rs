// SPDX-License-Identifier: MPL-2.0
//! A small x86-64 instruction encoder (Intel SDM volume 2): just what the
//! translator uses. Registers are numbered as in the encoding (RAX 0 ...
//! R15 15). Operand sizes are 1, 2, 4 or 8 bytes. The bytes go into a
//! vector; the dispatcher copies them into the code memory.

use alloc::vec::Vec;

pub type R = i32;

pub const RAX: R = 0;
pub const RCX: R = 1;
pub const RDX: R = 2;
pub const RBX: R = 3;
pub const RSP: R = 4;
pub const RBP: R = 5;
pub const RSI: R = 6;
pub const RDI: R = 7;
pub const R8: R = 8;
pub const R9: R = 9;
pub const R10: R = 10;
pub const R11: R = 11;
pub const R12: R = 12;
pub const R13: R = 13;
pub const R14: R = 14;
pub const R15: R = 15;

/// Condition codes (the low nibble of Jcc/SETcc/CMOVcc).
pub const XC_O: u8 = 0;
pub const XC_NO: u8 = 1;
pub const XC_B: u8 = 2;
pub const XC_AE: u8 = 3;
pub const XC_E: u8 = 4;
pub const XC_NE: u8 = 5;
pub const XC_BE: u8 = 6;
pub const XC_A: u8 = 7;
pub const XC_S: u8 = 8;
pub const XC_NS: u8 = 9;
pub const XC_L: u8 = 12;
pub const XC_GE: u8 = 13;
pub const XC_LE: u8 = 14;
pub const XC_G: u8 = 15;

/// ALU operations: the /digit of 80/81/83 and the row of 00-3F.
pub const XA_ADD: u8 = 0;
pub const XA_OR: u8 = 1;
pub const XA_ADC: u8 = 2;
pub const XA_SBB: u8 = 3;
pub const XA_AND: u8 = 4;
pub const XA_SUB: u8 = 5;
pub const XA_XOR: u8 = 6;
pub const XA_CMP: u8 = 7;

/// Shifts and rotates of r/m by an immediate or CL.
pub const XS_ROL: u8 = 0;
pub const XS_ROR: u8 = 1;
pub const XS_RCL: u8 = 2;
pub const XS_RCR: u8 = 3;
pub const XS_SHL: u8 = 4;
pub const XS_SHR: u8 = 5;
pub const XS_SAR: u8 = 7;

/// BT 4, BTS 5, BTR 6, BTC 7
pub const XB_BT: u8 = 4;
pub const XB_BTS: u8 = 5;
pub const XB_BTR: u8 = 6;
pub const XB_BTC: u8 = 7;

/// NOT 2, NEG 3, DIV 6, IDIV 7 (F6/F7 group)
pub const XU_NOT: u8 = 2;
pub const XU_NEG: u8 = 3;
pub const XU_DIV: u8 = 6;
pub const XU_IDIV: u8 = 7;

/// A memory operand: [base + index << scale + disp]. -1: none.
#[derive(Clone, Copy)]
pub struct Mem {
    pub base: R,
    pub index: R,
    pub scale: u8,
    pub disp: i32,
}

pub fn xm(base: R, disp: i32) -> Mem {
    Mem {
        base,
        index: -1,
        scale: 0,
        disp,
    }
}

pub fn xmi(base: R, index: R, scale: u8, disp: i32) -> Mem {
    Mem {
        base,
        index,
        scale,
        disp,
    }
}

fn fits8(v: i32) -> bool {
    (-128..=127).contains(&v)
}

fn low_byte_needs_rex(r: R) -> bool {
    (4..8).contains(&r)
}

#[derive(Default)]
pub struct XBuf {
    pub b: Vec<u8>,
}

impl XBuf {
    #[inline]
    pub fn pos(&self) -> usize {
        self.b.len()
    }

    #[inline]
    pub fn byte(&mut self, v: u8) {
        self.b.push(v);
    }

    pub fn word(&mut self, v: u16) {
        self.b.extend_from_slice(&v.to_le_bytes());
    }

    pub fn dword(&mut self, v: u32) {
        self.b.extend_from_slice(&v.to_le_bytes());
    }

    pub fn patch32(&mut self, at: usize, v: u32) {
        self.b[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }

    /// Prefixes: 66 for 16 bits, REX (W for 64 bits; R, X, B for the high
    /// registers; a bare REX when `force_rex`, for SPL/BPL/SIL/DIL).
    fn prefix(&mut self, size: u8, reg: R, index: R, base: R, force_rex: bool) {
        if size == 2 {
            self.byte(0x66);
        }
        let mut rex = 0u8;
        if size == 8 {
            rex |= 8;
        }
        if reg >= 8 {
            rex |= 4;
        }
        if index >= 8 {
            rex |= 2;
        }
        if base >= 8 {
            rex |= 1;
        }
        if rex != 0 || force_rex {
            self.byte(0x40 | rex);
        }
    }

    /// Opcode with a register operand in r/m. `byte_regs`: the operands are
    /// byte registers.
    fn rr(&mut self, size: u8, opc: &[u8], reg: R, rm: R, byte_regs: bool) {
        self.prefix(
            size,
            reg,
            -1,
            rm,
            byte_regs && (low_byte_needs_rex(reg) || low_byte_needs_rex(rm)),
        );
        self.b.extend_from_slice(opc);
        self.byte(0xC0 | ((reg & 7) as u8) << 3 | (rm & 7) as u8);
    }

    pub fn modrm_mem(&mut self, reg: R, m: Mem) {
        let reg = (reg & 7) as u8;
        if m.base < 0 {
            // [index*s + disp32] or [disp32]: SIB with no base
            self.byte(0x04 | reg << 3);
            let idx = if m.index < 0 { 4 } else { (m.index & 7) as u8 };
            let sc = if m.index < 0 { 0 } else { m.scale };
            self.byte(sc << 6 | idx << 3 | 5);
            self.dword(m.disp as u32);
            return;
        }
        let md: u8 = if m.disp == 0 && (m.base & 7) != 5 {
            0
        } else if fits8(m.disp) {
            1
        } else {
            2
        };
        let base = (m.base & 7) as u8;
        if m.index >= 0 || base == 4 {
            let idx = if m.index < 0 { 4 } else { (m.index & 7) as u8 };
            let sc = if m.index < 0 { 0 } else { m.scale };
            self.byte(md << 6 | reg << 3 | 4);
            self.byte(sc << 6 | idx << 3 | base);
        } else {
            self.byte(md << 6 | reg << 3 | base);
        }
        if md == 1 {
            self.byte(m.disp as i8 as u8);
        } else if md == 2 {
            self.dword(m.disp as u32);
        }
    }

    fn rm(&mut self, size: u8, opc: &[u8], reg: R, m: Mem, byte_reg: bool) {
        self.prefix(
            size,
            reg,
            m.index,
            if m.base < 0 { -1 } else { m.base },
            byte_reg && low_byte_needs_rex(reg),
        );
        self.b.extend_from_slice(opc);
        self.modrm_mem(reg, m);
    }

    // ---- Moves ----

    pub fn mov_rr(&mut self, size: u8, dst: R, src: R) {
        let o = if size == 1 { 0x88 } else { 0x89 };
        self.rr(size, &[o], src, dst, size == 1);
    }

    /// MOV r32, imm32
    pub fn mov_ri(&mut self, dst: R, imm: u32) {
        self.prefix(4, -1, -1, dst, false);
        self.byte(0xB8 + (dst & 7) as u8);
        self.dword(imm);
    }

    /// MOV r/m (size 1, 2 or 4), imm: only the low `size` bytes change.
    pub fn mov_ri_sz(&mut self, size: u8, dst: R, imm: u32) {
        if size == 4 {
            self.mov_ri(dst, imm);
            return;
        }
        let o = if size == 1 { 0xC6 } else { 0xC7 };
        self.rr(size, &[o], 0, dst, size == 1);
        if size == 1 {
            self.byte(imm as u8);
        } else {
            self.word(imm as u16);
        }
    }

    pub fn load(&mut self, size: u8, dst: R, m: Mem) {
        let o = if size == 1 { 0x8A } else { 0x8B };
        self.rm(size, &[o], dst, m, size == 1);
    }

    pub fn store(&mut self, size: u8, m: Mem, src: R) {
        let o = if size == 1 { 0x88 } else { 0x89 };
        self.rm(size, &[o], src, m, size == 1);
    }

    pub fn store_imm(&mut self, size: u8, m: Mem, imm: u32) {
        let o = if size == 1 { 0xC6 } else { 0xC7 };
        self.rm(size, &[o], 0, m, false);
        match size {
            1 => self.byte(imm as u8),
            2 => self.word(imm as u16),
            _ => self.dword(imm),
        }
    }

    /// MOVZX r32, r/m8 or r/m16
    pub fn movzx_load(&mut self, from: u8, dst: R, m: Mem) {
        let o = [0x0F, if from == 1 { 0xB6 } else { 0xB7 }];
        self.rm(4, &o, dst, m, false);
    }

    pub fn movzx_rr(&mut self, from: u8, dst: R, src: R) {
        let o = [0x0F, if from == 1 { 0xB6 } else { 0xB7 }];
        self.rr(4, &o, dst, src, from == 1);
    }

    pub fn movsx_rr(&mut self, from: u8, dst: R, src: R) {
        let o = [0x0F, if from == 1 { 0xBE } else { 0xBF }];
        self.rr(4, &o, dst, src, from == 1);
    }

    /// MOVBE r16/r32, m
    pub fn movbe_load(&mut self, size: u8, dst: R, m: Mem) {
        self.rm(size, &[0x0F, 0x38, 0xF0], dst, m, false);
    }

    /// MOVBE m, r16/r32
    pub fn movbe_store(&mut self, size: u8, m: Mem, src: R) {
        self.rm(size, &[0x0F, 0x38, 0xF1], src, m, false);
    }

    pub fn bswap32(&mut self, r: R) {
        self.prefix(4, -1, -1, r, false);
        self.byte(0x0F);
        self.byte(0xC8 + (r & 7) as u8);
    }

    /// ROL r16, 8: swaps the bytes of a word (changes CF and OF).
    pub fn rol16_8(&mut self, r: R) {
        self.rr(2, &[0xC1], 0, r, false);
        self.byte(8);
    }

    /// XCHG CL, CH: swaps the bytes of CX without touching the flags.
    pub fn xchg_cl_ch(&mut self) {
        self.byte(0x86);
        self.byte(0xE9);
    }

    /// LEA r32, [m]
    pub fn lea(&mut self, dst: R, m: Mem) {
        self.rm(4, &[0x8D], dst, m, false);
    }

    // ---- Arithmetic ----

    pub fn alu_rr(&mut self, op: u8, size: u8, dst: R, src: R) {
        let o = op << 3 | u8::from(size != 1);
        self.rr(size, &[o], src, dst, size == 1);
    }

    pub fn alu_ri(&mut self, op: u8, size: u8, dst: R, imm: u32) {
        if size == 1 {
            self.rr(1, &[0x80], R::from(op), dst, true);
            self.byte(imm as u8);
            return;
        }
        let s = if size == 2 {
            i32::from(imm as u16 as i16)
        } else {
            imm as i32
        };
        let short = fits8(s);
        self.rr(
            size,
            &[if short { 0x83 } else { 0x81 }],
            R::from(op),
            dst,
            false,
        );
        if short {
            self.byte(s as i8 as u8);
        } else if size == 2 {
            self.word(imm as u16);
        } else {
            self.dword(imm);
        }
    }

    pub fn alu_mi(&mut self, op: u8, size: u8, m: Mem, imm: i32) {
        if size == 1 {
            self.rm(1, &[0x80], R::from(op), m, false);
            self.byte(imm as u8);
            return;
        }
        let short = fits8(imm);
        self.rm(
            size,
            &[if short { 0x83 } else { 0x81 }],
            R::from(op),
            m,
            false,
        );
        if short {
            self.byte(imm as i8 as u8);
        } else if size == 2 {
            self.word(imm as u16);
        } else {
            self.dword(imm as u32);
        }
    }

    /// CMP r/m32, r32
    pub fn cmp_mr(&mut self, m: Mem, r: R) {
        self.rm(4, &[0x39], r, m, false);
    }

    pub fn test_rr(&mut self, size: u8, a: R, c: R) {
        let o = if size == 1 { 0x84 } else { 0x85 };
        self.rr(size, &[o], c, a, size == 1);
    }

    pub fn test_ri(&mut self, size: u8, r: R, imm: u32) {
        let o = if size == 1 { 0xF6 } else { 0xF7 };
        self.rr(size, &[o], 0, r, size == 1);
        match size {
            1 => self.byte(imm as u8),
            2 => self.word(imm as u16),
            _ => self.dword(imm),
        }
    }

    /// SHL (4), SHR (5) r32, imm
    pub fn shift_ri(&mut self, op: u8, r: R, n: u8) {
        self.rr(4, &[0xC1], R::from(op), r, false);
        self.byte(n);
    }

    /// BT r32, r32: CF = bit `bit` of `base`.
    pub fn bt_rr(&mut self, base: R, bit: R) {
        self.rr(4, &[0x0F, 0xA3], bit, base, false);
    }

    pub fn pushfq(&mut self) {
        self.byte(0x9C);
    }

    /// LAHF, SAHF: AH and SF ZF AF PF CF (the LAHF-SAHF CPUID bit).
    pub fn lahf(&mut self) {
        self.byte(0x9F);
    }

    pub fn sahf(&mut self) {
        self.byte(0x9E);
    }

    /// MOV [m], AH: no REX, so m's registers must be among RAX-RDI.
    pub fn store_ah(&mut self, m: Mem) {
        self.byte(0x88);
        self.modrm_mem(4, m);
    }

    /// TEST dword [m], imm32
    pub fn test_mi32(&mut self, m: Mem, imm: u32) {
        self.rm(4, &[0xF7], 0, m, false);
        self.dword(imm);
    }

    pub fn push(&mut self, r: R) {
        if r >= 8 {
            self.byte(0x41);
        }
        self.byte(0x50 + (r & 7) as u8);
    }

    pub fn pop(&mut self, r: R) {
        if r >= 8 {
            self.byte(0x41);
        }
        self.byte(0x58 + (r & 7) as u8);
    }

    pub fn ret(&mut self) {
        self.byte(0xC3);
    }

    /// SUB/ADD RSP, imm
    pub fn rsp_adjust(&mut self, v: i32) {
        self.alu_ri(
            if v < 0 { XA_SUB } else { XA_ADD },
            8,
            RSP,
            v.unsigned_abs(),
        );
    }

    // ---- Jumps ----

    /// Jcc rel32 with a target to fill in later: returns the position of
    /// the rel32 field.
    pub fn jcc32(&mut self, cc: u8) -> usize {
        self.byte(0x0F);
        self.byte(0x80 + cc);
        let at = self.pos();
        self.dword(0);
        at
    }

    pub fn jmp32(&mut self) -> usize {
        self.byte(0xE9);
        let at = self.pos();
        self.dword(0);
        at
    }

    /// Points the rel32 field at `at` to position `to` of the buffer.
    pub fn link(&mut self, at: usize, to: i64) {
        self.patch32(at, (to - (at as i64 + 4)) as u32);
    }

    /// JMP r64
    pub fn jmp_r(&mut self, r: R) {
        self.rr(4, &[0xFF], 4, r, false);
    }

    /// JMP [m]
    pub fn jmp_m(&mut self, m: Mem) {
        self.rm(4, &[0xFF], 4, m, false);
    }

    /// CALL r64
    pub fn call_r(&mut self, r: R) {
        self.rr(4, &[0xFF], 2, r, false);
    }

    /// CALL rel32 with a target to fill in later.
    pub fn call32(&mut self) -> usize {
        self.byte(0xE8);
        let at = self.pos();
        self.dword(0);
        at
    }

    /// MOV r64, imm64
    pub fn mov_ri64(&mut self, dst: R, imm: u64) {
        self.prefix(8, -1, -1, dst, false);
        self.byte(0xB8 + (dst & 7) as u8);
        self.dword(imm as u32);
        self.dword((imm >> 32) as u32);
    }

    /// PUSH imm32 (sign-extended)
    pub fn push_imm(&mut self, v: i32) {
        self.byte(0x68);
        self.dword(v as u32);
    }

    /// LEA r64, [m] (64-bit result: for RSP)
    pub fn lea64(&mut self, dst: R, m: Mem) {
        self.rm(8, &[0x8D], dst, m, false);
    }

    /// ADD r64, [m]
    pub fn add_rm64(&mut self, dst: R, m: Mem) {
        self.rm(8, &[0x03], dst, m, false);
    }

    /// MOV r64, [m]
    pub fn load64(&mut self, dst: R, m: Mem) {
        self.rm(8, &[0x8B], dst, m, false);
    }

    /// TEST byte [m], imm8
    pub fn test_mi8(&mut self, m: Mem, imm: u8) {
        self.rm(1, &[0xF6], 0, m, false);
        self.byte(imm);
    }

    pub fn shift_r(&mut self, op: u8, size: u8, r: R, n: u8) {
        let o = if size == 1 { 0xC0 } else { 0xC1 };
        self.rr(size, &[o], R::from(op), r, size == 1);
        self.byte(n);
    }

    pub fn btx_ri(&mut self, op: u8, r: R, bit: u8) {
        self.rr(4, &[0x0F, 0xBA], R::from(op), r, false);
        self.byte(bit);
    }

    pub fn btx_rr(&mut self, op: u8, base: R, bit: R) {
        const CODES: [u8; 4] = [0xA3, 0xAB, 0xB3, 0xBB];
        self.rr(4, &[0x0F, CODES[usize::from(op - XB_BT)]], bit, base, false);
    }

    /// SETcc r8
    pub fn setcc(&mut self, cc: u8, r: R) {
        self.rr(1, &[0x0F, 0x90 + cc], 0, r, true);
    }

    pub fn unary(&mut self, op: u8, size: u8, r: R) {
        let o = if size == 1 { 0xF6 } else { 0xF7 };
        self.rr(size, &[o], R::from(op), r, size == 1);
    }

    /// IMUL r, r/m (size 2, 4 or 8)
    pub fn imul_rr(&mut self, size: u8, dst: R, src: R) {
        self.rr(size, &[0x0F, 0xAF], dst, src, false);
    }

    pub fn cqo(&mut self) {
        self.byte(0x48);
        self.byte(0x99);
    }

    /// XCHG r32, r32
    pub fn xchg_rr(&mut self, a: R, c: R) {
        self.rr(4, &[0x87], a, c, false);
    }

    /// MOVSXD r64, r32
    pub fn movsxd(&mut self, dst: R, src: R) {
        self.rr(8, &[0x63], dst, src, false);
    }

    /// MOVSX r64, r8/r16
    pub fn movsx64_rr(&mut self, from: u8, dst: R, src: R) {
        let o = [0x0F, if from == 1 { 0xBE } else { 0xBF }];
        self.rr(8, &o, dst, src, from == 1);
    }
}
