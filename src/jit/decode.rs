// SPDX-License-Identifier: MPL-2.0
//! The JIT's 68k decoder: what an instruction does, in a form the code
//! generator uses directly. It only accepts what the generator translates;
//! everything else is left to the interpreter.
//!
//! The encodings are those of M68000PRM chapter 4 and, for the 68020's
//! extension words, the 32-bit branch displacement and its new
//! instructions, MC68020UM. The effective address classes are the
//! interpreter's, and an opcode the interpreter's table rejects on this
//! model is never translated.

use crate::cpu::{Fpu, Model};
use crate::interp::{
    EA_ALL, EA_CONTROL, EA_CONTROL_ALT, EA_DATA, EA_DATA_ALT, EA_MEM_ALT, EAB_AREG, EAB_IMM,
    EAB_PCD8, EAB_PCD16, EAB_POSTINC, EAB_PREDEC, cond_true, ea_valid, size_field, sz_mask,
};
use crate::window::{PAGE_NONE, PAGE_RAM, PAGE_RAM_CODE, PAGE_ROM, Window, direct_read, in_page};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Kind {
    #[default]
    Nop,
    /// src -> dst (Dn or memory); N Z, V C cleared
    Move,
    /// src (.W sign-extended) -> An
    Movea,
    /// imm -> Dn
    Moveq,
    /// address of src -> An
    Lea,
    /// push the address of src
    Pea,
    /// dst = dst op src (CMP: no write)
    Alu,
    /// An = An op src (.W sign-extended); CMPA: flags
    Adda,
    /// CMP (Ay)+,(Ax)+
    Cmpm,
    Tst,
    Clr,
    /// NEG, NEGX, NOT on dst (sub: U_*)
    Unary,
    /// ADDX/SUBX src,dst (sub: 0 add, 1 sub)
    Addx,
    /// shifts and rotates on Dn by #1-8, or memory by 1
    Shift,
    /// BTST BCHG BCLR BSET #n or Dn (src) on dst
    Bit,
    /// MULU.W/MULS.W src,Dn (sub: signed)
    Mul,
    /// DIVU.W/DIVS.W src,Dn (sub: signed)
    Div,
    /// MULx.L (ext: the extension word)
    Mull,
    /// DIVx.L
    Divl,
    /// EXT.W, EXT.L, EXTB.L (sub: E_*) on Dn
    Ext,
    Swap,
    /// src <-> dst registers
    Exg,
    Scc,
    /// mask: the register list; sub: 1 memory to registers
    Movem,
    /// LINK An (dst),#imm (src)
    Link,
    Unlk,
    /// TAS dst (a byte)
    Tas,
    /// CHK src,Dn (dst), .W or .L
    Chk,
    /// CAS Dc,Du,dst (ext: the extension word)
    Cas,
    /// MOVEP between Dn and (d16,Ay); sub 1: to memory
    Movep,
    /// The interpreter's handler, called from the block; memory it touches
    /// is checked first: sub 1, a bit field (dst, ext; 5 bytes, `write`);
    /// sub 2, ABCD or SBCD -(Ay),-(Ax) (src, dst); sub 3, NBCD in memory
    /// (dst)
    Call,
    /// An FPU instruction, by fpu_jit_op(): ext the command word, dst the
    /// operand, size its bytes
    Fpu,
    /// FBcc (cc: the predicate)
    Fbcc,
    /// Bcc and BRA (cc 0)
    Bcc,
    Bsr,
    Dbcc,
    Jmp,
    Jsr,
    Rts,
}

pub const A_ADD: u8 = 0;
pub const A_SUB: u8 = 1;
pub const A_CMP: u8 = 2;
pub const A_AND: u8 = 3;
pub const A_OR: u8 = 4;
pub const A_EOR: u8 = 5;

pub const U_NEGX: u8 = 0;
pub const U_NEG: u8 = 1;
pub const U_NOT: u8 = 2;

pub const S_AS: u8 = 0;
pub const S_LS: u8 = 1;
pub const S_ROX: u8 = 2;
pub const S_RO: u8 = 3;

pub const B_TST: u8 = 0;

pub const E_W: u8 = 0;
pub const E_L: u8 = 1;
pub const E_BL: u8 = 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum EaKind {
    #[default]
    None,
    Dreg,
    Areg,
    Mem,
    Imm,
}

/// Registers are numbered 0-15: D0-D7, then A0-A7.
pub const fn ra(n: u32) -> u8 {
    (n + 8) as u8
}

#[derive(Clone, Copy, Debug)]
pub struct Ea {
    pub kind: EaKind,
    /// Dreg/Areg: 0-15 (as ra() for An)
    pub reg: u8,
    /// Mem: register 0-15, or -1
    pub base: i8,
    /// Mem: register 0-15, or -1
    pub index: i8,
    /// index is Xn.L (else Xn.W)
    pub index_long: bool,
    /// index shift, 0-3
    pub scale: u8,
    /// (An)+: +bytes, -(An): -bytes, else 0
    pub step: i8,
    /// Mem, 68020 memory indirect: 0 none, 1 the index (if any) is added
    /// before the indirection, 2 after it.
    pub ind: u8,
    /// Mem at a constant address that is not plain but cannot fault (a host
    /// page, a ROM write): the code always calls the interpreter.
    pub host: bool,
    /// Mem: displacement; the whole address if there is neither base nor
    /// index. Imm: the value.
    pub disp: u32,
    /// Mem with ind: the outer displacement
    pub od: u32,
}

impl Default for Ea {
    fn default() -> Ea {
        Ea {
            kind: EaKind::None,
            reg: 0,
            base: -1,
            index: -1,
            index_long: false,
            scale: 0,
            step: 0,
            ind: 0,
            host: false,
            disp: 0,
            od: 0,
        }
    }
}

impl Ea {
    fn dreg(reg: u32) -> Ea {
        Ea {
            kind: EaKind::Dreg,
            reg: reg as u8,
            ..Ea::default()
        }
    }

    fn areg(reg: u32) -> Ea {
        Ea {
            kind: EaKind::Areg,
            reg: ra(reg),
            ..Ea::default()
        }
    }

    fn imm(v: u32) -> Ea {
        Ea {
            kind: EaKind::Imm,
            disp: v,
            ..Ea::default()
        }
    }

    /// A memory operand at an address known when translating.
    pub fn mem_const(&self) -> bool {
        self.kind == EaKind::Mem && self.base < 0 && self.index < 0 && self.ind == 0
    }

    /// The registers it reads.
    fn regs(&self) -> u16 {
        let mut m = 0;
        match self.kind {
            EaKind::Dreg | EaKind::Areg => m |= 1 << self.reg,
            EaKind::Mem => {
                if self.base >= 0 {
                    m |= 1 << self.base;
                }
                if self.index >= 0 {
                    m |= 1 << self.index;
                }
            }
            _ => {}
        }
        m
    }

    fn reg_of(&self) -> u16 {
        match self.kind {
            EaKind::Dreg | EaKind::Areg => 1 << self.reg,
            _ => 0,
        }
    }

    fn steps(&self) -> u16 {
        if self.kind == EaKind::Mem && self.step != 0 {
            1 << self.base
        } else {
            0
        }
    }
}

/// CCR bits as in SR.
pub const F_C: u8 = 0x01;
pub const F_V: u8 = 0x02;
pub const F_Z: u8 = 0x04;
pub const F_N: u8 = 0x08;
pub const F_X: u8 = 0x10;
pub const F_NZVC: u8 = 0x0F;
pub const F_ALL: u8 = 0x1F;

#[derive(Clone, Copy, Debug, Default)]
pub struct Insn {
    pub pc: u32,
    pub op: u16,
    /// bytes
    pub len: u32,
    pub kind: Kind,
    /// A_* (Alu, Adda)
    pub alu: u8,
    /// the variant, by kind (see Kind)
    pub sub: u8,
    /// operation size: 1, 2, 4
    pub size: u32,
    /// Bcc, Dbcc, Scc, Fbcc
    pub cc: u32,
    /// CCR bits it sets
    pub flags_def: u8,
    /// CCR bits it reads
    pub flags_use: u8,
    /// has a check that can send it to the interpreter
    pub may_bail: bool,
    pub ends_block: bool,
    /// Shift
    pub left: bool,
    /// Call with sub 1, Fpu
    pub write: bool,
    /// Shift: 1-8
    pub count: u8,
    /// registers it needs in x86 registers
    pub regs: u16,
    /// registers it changes (also in memory)
    pub wregs: u16,
    /// Movem
    pub mask: u16,
    /// Mull, Divl, Cas, Call, Fpu: the extension word
    pub ext: u16,
    /// rough cycle count
    pub cycles: u32,
    pub src: Ea,
    pub dst: Ea,
    /// Bcc, Bsr, Dbcc, Fbcc
    pub target: u32,
}

/// What the decoder needs to know of the CPU.
pub struct Ctx<'a> {
    pub mem: &'a Window,
    pub addr_mask: u32,
    pub model: Model,
    pub fpu: Fpu,
    pub align_check: bool,
    /// Opcodes the interpreter's table gives a handler on this model
    pub legal: &'a [bool],
}

struct Reader<'a, 'b> {
    c: &'b Ctx<'a>,
    pc: u32,
    ok: bool,
    rom: &'b mut bool,
}

impl Reader<'_, '_> {
    fn rd16(&mut self) -> u32 {
        if !self.ok {
            return 0;
        }
        let c = self.c;
        let a = self.pc & c.addr_mask;
        let kind = c.mem.kind(a);
        if !direct_read(kind) || u64::from(a) + 2 > u64::from(c.addr_mask) + 1 {
            self.ok = false;
            return 0;
        }
        if kind != PAGE_ROM {
            *self.rom = false;
        }
        self.pc = self.pc.wrapping_add(2);
        // An even address: never across a page.
        c.mem.read16(a).unwrap_or(0)
    }

    fn rd32(&mut self) -> u32 {
        let hi = self.rd16();
        hi << 16 | self.rd16()
    }

    fn m020(&self) -> bool {
        self.c.model >= Model::M68020
    }
}

fn sx8(v: u32) -> u32 {
    v as u8 as i8 as i32 as u32
}

fn sx16(v: u32) -> u32 {
    v as u16 as i16 as i32 as u32
}

/// The 68020's full extension word (MC68020UM 2.2), as the interpreter
/// reads it: the reserved encodings are its business.
fn full_format(r: &mut Reader, e: &mut Ea, base: i32, pc_ext: u32, ext: u32) -> bool {
    let iis = ext & 7;
    let is = ext & 0x0040 != 0;
    if ext & 0x0030 == 0 || iis == 4 || (is && iis > 4) {
        return false;
    }
    let mut bd = 0;
    let mut od = 0;
    match (ext >> 4) & 3 {
        2 => bd = sx16(r.rd16()),
        3 => bd = r.rd32(),
        _ => {}
    }
    match iis & 3 {
        2 => od = sx16(r.rd16()),
        3 => od = r.rd32(),
        _ => {}
    }
    e.kind = EaKind::Mem;
    e.base = -1;
    e.disp = bd;
    if ext & 0x0080 == 0 {
        if base >= 0 {
            e.base = base as i8;
        } else {
            e.disp = pc_ext.wrapping_add(bd);
        }
    }
    if !is {
        e.index = (((ext >> 12) & 7) + if ext & 0x8000 != 0 { 8 } else { 0 }) as i8;
        e.index_long = ext & 0x0800 != 0;
        e.scale = ((ext >> 9) & 3) as u8;
    }
    e.ind = if iis == 0 {
        0
    } else if iis & 4 != 0 {
        2
    } else {
        1
    };
    e.od = od;
    true
}

/// (d8,An,Xn) and (d8,PC,Xn): the brief extension word or, from the 68020
/// on, the full one. `base` is An as 8-15, or -1 for the PC (whose value,
/// that of the extension word, is `pc_ext`).
fn indexed(r: &mut Reader, e: &mut Ea, base: i32, pc_ext: u32) -> bool {
    let ext = r.rd16();
    let m020 = r.m020();
    if m020 && ext & 0x0100 != 0 {
        return full_format(r, e, base, pc_ext, ext);
    }
    e.kind = EaKind::Mem;
    e.base = base as i8;
    e.index = (((ext >> 12) & 7) + if ext & 0x8000 != 0 { 8 } else { 0 }) as i8;
    e.index_long = ext & 0x0800 != 0;
    e.scale = if m020 { ((ext >> 9) & 3) as u8 } else { 0 };
    e.disp = (if base < 0 { pc_ext } else { 0 }).wrapping_add(sx8(ext));
    true
}

fn dec_ea(r: &mut Reader, e: &mut Ea, mode: u32, reg: u32, size: u32) -> bool {
    *e = Ea::default();
    let step = if size == 1 && reg == 7 { 2 } else { size as i8 };
    match mode {
        0 => {
            *e = Ea::dreg(reg);
            return true;
        }
        1 => {
            *e = Ea::areg(reg);
            return true;
        }
        2..=4 => {
            e.kind = EaKind::Mem;
            e.base = ra(reg) as i8;
            e.step = match mode {
                3 => step,
                4 => -step,
                _ => 0,
            };
            return true;
        }
        5 => {
            e.kind = EaKind::Mem;
            e.base = ra(reg) as i8;
            e.disp = sx16(r.rd16());
            return true;
        }
        6 => return indexed(r, e, i32::from(ra(reg)), 0),
        _ => {}
    }
    let pc_ext = r.pc;
    match reg {
        0 => {
            e.kind = EaKind::Mem;
            e.disp = sx16(r.rd16());
            true
        }
        1 => {
            e.kind = EaKind::Mem;
            e.disp = r.rd32();
            true
        }
        2 => {
            e.kind = EaKind::Mem;
            e.disp = pc_ext.wrapping_add(sx16(r.rd16()));
            true
        }
        3 => indexed(r, e, -1, pc_ext),
        4 => {
            e.kind = EaKind::Imm;
            e.disp = if size == 4 {
                r.rd32()
            } else {
                r.rd16() & sz_mask(size)
            };
            true
        }
        _ => false,
    }
}

fn dec_ea_class(r: &mut Reader, e: &mut Ea, mode: u32, reg: u32, size: u32, classes: u32) -> bool {
    if !ea_valid(mode, reg, classes) {
        return false;
    }
    dec_ea(r, e, mode, reg, size)
}

/// CCR bits a condition reads.
fn cc_flags(cc: u32) -> u8 {
    match cc & 15 {
        0 | 1 => 0,
        2 | 3 => F_C | F_Z,
        4 | 5 => F_C,
        6 | 7 => F_Z,
        8 | 9 => F_V,
        10 | 11 => F_N,
        12 | 13 => F_N | F_V,
        _ => F_N | F_V | F_Z,
    }
}

/// An instruction run by its interpreter handler, called from the block:
/// only register operands, no exception possible. `wregs`: what it may
/// change.
fn call_handler(i: &mut Insn, wregs: u16) -> bool {
    i.kind = Kind::Call;
    i.wregs = wregs;
    true
}

fn op_mode(op: u16) -> u32 {
    u32::from((op >> 3) & 7)
}

fn op_reg(op: u16) -> u32 {
    u32::from(op & 7)
}

fn op_reg9(op: u16) -> u32 {
    u32::from((op >> 9) & 7)
}

// ---- Lines -------------------------------------------------------------------

/// BTST, BCHG, BCLR, BSET: 0000 rrr 1tt <ea> with the bit number in Dn,
/// 0000 1000 tt <ea> with it in the next word. On Dn the operand is a long
/// (bit modulo 32), in memory a byte (modulo 8).
fn dec_bit(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let typ = u32::from((op >> 6) & 3);
    let mode = op_mode(op);
    i.kind = Kind::Bit;
    i.sub = typ as u8;
    let classes = if op & 0x0100 != 0 {
        i.src = Ea::dreg(op_reg9(op));
        if typ == u32::from(B_TST) {
            EA_DATA
        } else {
            EA_DATA_ALT
        }
    } else {
        i.src = Ea::imm(r.rd16() & 0xFF);
        if typ == u32::from(B_TST) {
            EA_DATA & !EAB_IMM
        } else {
            EA_DATA_ALT
        }
    };
    i.size = if mode == 0 { 4 } else { 1 };
    dec_ea_class(r, &mut i.dst, mode, op_reg(op), i.size, classes)
}

/// Line 0: bit operations, and ORI, ANDI, SUBI, ADDI, EORI, CMPI to a data
/// alterable operand (CMPI also relative to the PC on the 68020).
fn dec_line0(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    const ALU: [i8; 8] = [
        A_OR as i8,
        A_AND as i8,
        A_SUB as i8,
        A_ADD as i8,
        -1,
        A_EOR as i8,
        A_CMP as i8,
        -1,
    ];
    if op & 0x0138 == 0x0108 {
        // MOVEP: 0000 ddd 1 ds 001 aaa, a displacement word
        i.kind = Kind::Movep;
        i.sub = u8::from(op & 0x0080 != 0);
        i.size = if op & 0x0040 != 0 { 4 } else { 2 };
        i.src = Ea::dreg(op_reg9(op));
        return dec_ea(r, &mut i.dst, 5, op_reg(op), 1);
    }
    if op & 0x0100 != 0 || op & 0x0F00 == 0x0800 {
        return dec_bit(r, i, op);
    }
    let size = size_field(op);
    if size == 0 && op & 0x0800 != 0 && (op >> 9) & 3 != 0 && r.m020() {
        // CAS Dc,Du,<ea>: 0000 1ss 011 <ea>, ss 01 B 10 W 11 L
        const SIZES: [u32; 4] = [0, 1, 2, 4];
        i.kind = Kind::Cas;
        i.size = SIZES[usize::from((op >> 9) & 3)];
        i.ext = r.rd16() as u16;
        return dec_ea_class(r, &mut i.dst, op_mode(op), op_reg(op), i.size, EA_MEM_ALT);
    }
    let a = ALU[op_reg9(op) as usize];
    if size == 0 || a < 0 {
        return false;
    }
    let imm = if size == 4 {
        r.rd32()
    } else {
        r.rd16() & sz_mask(size)
    };
    i.kind = Kind::Alu;
    i.alu = a as u8;
    i.size = size;
    i.src = Ea::imm(imm);
    let mut classes = EA_DATA_ALT;
    if a as u8 == A_CMP && r.m020() {
        classes |= EAB_PCD16 | EAB_PCD8;
    }
    dec_ea_class(r, &mut i.dst, op_mode(op), op_reg(op), size, classes)
}

fn dec_move(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    const SIZES: [u32; 4] = [0, 1, 4, 2];
    let size = SIZES[usize::from((op >> 12) & 3)];
    let dmode = u32::from((op >> 6) & 7);
    let dreg = op_reg9(op);
    i.size = size;
    let classes = if size == 1 { EA_DATA } else { EA_ALL };
    if !dec_ea_class(r, &mut i.src, op_mode(op), op_reg(op), size, classes) {
        return false;
    }
    if dmode == 1 {
        if size == 1 {
            return false;
        }
        i.kind = Kind::Movea;
        i.dst = Ea::areg(dreg);
        return true;
    }
    i.kind = Kind::Move;
    dec_ea_class(r, &mut i.dst, dmode, dreg, size, EA_DATA_ALT)
}

fn dec_line4(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let mode = op_mode(op);
    let reg = op_reg(op);
    let size = size_field(op);
    let m020 = r.m020();
    if op == 0x4E71 {
        i.kind = Kind::Nop;
        return true;
    }
    if op == 0x4E75 {
        i.kind = Kind::Rts;
        return true;
    }
    if op & 0xFF80 == 0x4E80 {
        // JSR 4E80, JMP 4EC0
        i.kind = if op & 0x0040 != 0 {
            Kind::Jmp
        } else {
            Kind::Jsr
        };
        i.size = 4;
        return dec_ea_class(r, &mut i.src, mode, reg, 4, EA_CONTROL);
    }
    if op & 0xFFF8 == 0x4E50 || op & 0xFFF8 == 0x4808 {
        // LINK.W, LINK.L (68020)
        i.kind = Kind::Link;
        i.dst = Ea::areg(reg);
        i.src = Ea::imm(if op & 0x0040 != 0 {
            sx16(r.rd16())
        } else {
            r.rd32()
        });
        return true;
    }
    if op & 0xFFF8 == 0x4E58 {
        i.kind = Kind::Unlk;
        i.dst = Ea::areg(reg);
        return true;
    }
    if op & 0xF1C0 == 0x41C0 {
        i.kind = Kind::Lea;
        i.size = 4;
        i.dst = Ea::areg(op_reg9(op));
        return dec_ea_class(r, &mut i.src, mode, reg, 4, EA_CONTROL);
    }
    if op & 0xFFF8 == 0x4840 {
        i.kind = Kind::Swap;
        i.dst = Ea::dreg(reg);
        return true;
    }
    if op & 0xFFC0 == 0x4840 {
        i.kind = Kind::Pea;
        i.size = 4;
        return dec_ea_class(r, &mut i.src, mode, reg, 4, EA_CONTROL);
    }
    if op & 0xFFF8 == 0x4880 || op & 0xFFF8 == 0x48C0 || op & 0xFFF8 == 0x49C0 {
        i.kind = Kind::Ext;
        i.sub = if op & 0x0100 != 0 {
            E_BL
        } else if op & 0x0040 != 0 {
            E_L
        } else {
            E_W
        };
        i.dst = Ea::dreg(reg);
        return true;
    }
    if op & 0xFB80 == 0x4880 {
        // MOVEM
        let to_regs = op & 0x0400 != 0;
        i.kind = Kind::Movem;
        i.sub = u8::from(to_regs);
        i.size = if op & 0x0040 != 0 { 4 } else { 2 };
        i.mask = r.rd16() as u16;
        if i.mask == 0 {
            return false;
        }
        let classes = if to_regs {
            EA_CONTROL | EAB_POSTINC
        } else {
            EA_CONTROL_ALT | EAB_PREDEC
        };
        let size = i.size;
        let e = if to_regs { &mut i.src } else { &mut i.dst };
        if !dec_ea_class(r, e, mode, reg, size, classes) {
            return false;
        }
        e.step = 0; // MOVEM moves An itself
        return true;
    }
    if op & 0xFF80 == 0x4C00 {
        // MULx.L, DIVx.L
        i.kind = if op & 0x0040 != 0 {
            Kind::Divl
        } else {
            Kind::Mull
        };
        i.ext = r.rd16() as u16;
        i.size = 4;
        return dec_ea_class(r, &mut i.src, mode, reg, 4, EA_DATA);
    }
    if op & 0xFFF8 == 0x4800 {
        // NBCD Dn
        return call_handler(i, 1 << reg);
    }
    if op & 0xFFC0 == 0x4800 {
        // NBCD <ea>, checked first
        if !dec_ea_class(r, &mut i.dst, mode, reg, 1, EA_DATA_ALT) {
            return false;
        }
        call_handler(i, 0);
        i.sub = 3;
        i.size = 1;
        i.write = true;
        return true;
    }
    if op & 0xFFC0 == 0x4AC0 {
        i.kind = Kind::Tas;
        i.size = 1;
        return dec_ea_class(r, &mut i.dst, mode, reg, 1, EA_DATA_ALT);
    }
    if op & 0xF1C0 == 0x4180 || (op & 0xF1C0 == 0x4100 && m020) {
        // CHK.W, CHK.L
        i.kind = Kind::Chk;
        i.size = if op & 0x0080 != 0 { 2 } else { 4 };
        i.dst = Ea::dreg(op_reg9(op));
        return dec_ea_class(r, &mut i.src, mode, reg, i.size, EA_DATA);
    }
    if op & 0xF900 == 0x4000 && size != 0 {
        // NEGX 40, CLR 42, NEG 44, NOT 46
        let k = (op >> 9) & 3;
        i.size = size;
        if k == 1 {
            i.kind = Kind::Clr;
        } else {
            i.kind = Kind::Unary;
            i.sub = match k {
                0 => U_NEGX,
                2 => U_NEG,
                _ => U_NOT,
            };
        }
        return dec_ea_class(r, &mut i.dst, mode, reg, size, EA_DATA_ALT);
    }
    if op & 0xFF00 == 0x4A00 && size != 0 {
        i.kind = Kind::Tst;
        i.size = size;
        let mut classes = EA_DATA_ALT;
        if m020 {
            classes |= EAB_PCD16 | EAB_PCD8 | EAB_IMM | if size != 1 { EAB_AREG } else { 0 };
        }
        return dec_ea_class(r, &mut i.src, mode, reg, size, classes);
    }
    false
}

/// ADDQ, SUBQ, DBcc and Scc.
fn dec_line5(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let mode = op_mode(op);
    let reg = op_reg(op);
    let size = size_field(op);
    if size == 0 {
        i.cc = u32::from((op >> 8) & 15);
        if mode != 1 {
            i.kind = Kind::Scc;
            i.size = 1;
            return dec_ea_class(r, &mut i.dst, mode, reg, 1, EA_DATA_ALT);
        }
        i.kind = Kind::Dbcc;
        i.target = i.pc.wrapping_add(2).wrapping_add(sx16(r.rd16()));
        i.dst = Ea::dreg(reg);
        return i.target & 1 == 0;
    }
    let mut q = op_reg9(op);
    if q == 0 {
        q = 8;
    }
    i.alu = if op & 0x0100 != 0 { A_SUB } else { A_ADD };
    if mode == 1 {
        if size == 1 {
            return false;
        }
        i.kind = Kind::Adda; // the whole An, no flags
        i.size = 4;
        i.src = Ea::imm(q);
        i.dst = Ea::areg(reg);
        return true;
    }
    i.kind = Kind::Alu;
    i.size = size;
    i.src = Ea::imm(q);
    dec_ea_class(r, &mut i.dst, mode, reg, size, EA_DATA_ALT)
}

fn dec_branch(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let base = i.pc.wrapping_add(2);
    let disp = u32::from(op & 0xFF);
    i.target = if disp == 0 {
        base.wrapping_add(sx16(r.rd16()))
    } else if disp == 0xFF && r.m020() {
        base.wrapping_add(r.rd32())
    } else {
        base.wrapping_add(sx8(disp))
    };
    if i.target & 1 != 0 {
        return false; // always an address error: the interpreter's
    }
    i.cc = u32::from((op >> 8) & 15);
    i.kind = if i.cc == 1 { Kind::Bsr } else { Kind::Bcc };
    true
}

fn mem_ea(base: u8, step: i8) -> Ea {
    Ea {
        kind: EaKind::Mem,
        base: base as i8,
        step,
        ..Ea::default()
    }
}

/// ADDX/SUBX (lines D/9) and CMPM (line B): 1101 xxx 1 ss 00 r yyy.
fn dec_xy(i: &mut Insn, op: u16, line: u16) -> bool {
    let size = size_field(op);
    let rx = op_reg9(op);
    let ry = op_reg(op);
    let mem = op & 8 != 0;
    i.size = size;
    let step = |r: u32| -> i8 { if size == 1 && r == 7 { 2 } else { size as i8 } };
    if line == 0xB {
        if !mem {
            return false; // EOR Dn,An does not exist
        }
        i.kind = Kind::Cmpm;
        i.src = mem_ea(ra(ry), step(ry));
        i.dst = mem_ea(ra(rx), step(rx));
        return true;
    }
    i.kind = Kind::Addx;
    i.sub = u8::from(line == 0x9);
    if !mem {
        i.src = Ea::dreg(ry);
        i.dst = Ea::dreg(rx);
        return true;
    }
    i.src = mem_ea(ra(ry), -step(ry));
    i.dst = mem_ea(ra(rx), -step(rx));
    true
}

/// Lines 8 (OR, DIVx, SBCD, PACK, UNPK), 9 (SUB, SUBX), B (CMP, CMPA, EOR,
/// CMPM), C (AND, MULx, ABCD, EXG), D (ADD, ADDX).
fn dec_alu_line(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let line = op >> 12;
    let opmode = u32::from((op >> 6) & 7);
    let mode = op_mode(op);
    let reg = op_reg(op);
    let dn = op_reg9(op);
    let arith = line == 0x9 || line == 0xD || line == 0xB;
    i.alu = match line {
        0x8 => A_OR,
        0x9 => A_SUB,
        0xB => A_CMP,
        0xC => A_AND,
        _ => A_ADD,
    };
    if opmode == 3 || opmode == 7 {
        if line == 0x8 || line == 0xC {
            // DIVU/DIVS, MULU/MULS .W
            i.kind = if line == 0x8 { Kind::Div } else { Kind::Mul };
            i.sub = u8::from(opmode == 7);
            i.size = 2;
            i.dst = Ea::dreg(dn);
            if !dec_ea_class(r, &mut i.src, mode, reg, 2, EA_DATA) {
                return false;
            }
            // a constant 0 divisor always traps
            return !(i.kind == Kind::Div && i.src.kind == EaKind::Imm && i.src.disp == 0);
        }
        let size = if opmode == 7 { 4 } else { 2 };
        i.kind = Kind::Adda;
        i.size = size;
        i.dst = Ea::areg(dn);
        return dec_ea_class(r, &mut i.src, mode, reg, size, EA_ALL);
    }
    let size = [1, 2, 4][(opmode & 3) as usize];
    i.kind = Kind::Alu;
    i.size = size;
    if opmode < 3 {
        // <ea>,Dn
        i.dst = Ea::dreg(dn);
        let classes = if arith && size != 1 { EA_ALL } else { EA_DATA };
        return dec_ea_class(r, &mut i.src, mode, reg, size, classes);
    }
    // Dn,<ea>, or the register-to-register forms in modes 0 and 1
    if mode <= 1 {
        if line == 0x9 || line == 0xD || (line == 0xB && mode == 1) {
            return dec_xy(i, op, line);
        }
        if (line == 0x8 || line == 0xC) && opmode == 4 && mode == 0 {
            return call_handler(i, 1 << dn); // SBCD ABCD
        }
        if (line == 0x8 || line == 0xC) && opmode == 4 {
            // SBCD ABCD -(Ay),-(Ax): the bytes are checked first
            call_handler(i, 1 << ra(dn) | 1 << ra(reg));
            i.sub = 2;
            i.src = Ea::areg(reg);
            i.dst = Ea::areg(dn);
            return true;
        }
        if line == 0x8 && (opmode == 5 || opmode == 6) && mode == 0 {
            r.rd16(); // PACK UNPK
            return call_handler(i, 1 << dn);
        }
        if line == 0xC && (opmode == 5 || opmode == 6) {
            // EXG Dx,Dy (01000), Ax,Ay (01001), Dx,Ay (10001)
            i.kind = Kind::Exg;
            match (op >> 3) & 0x1F {
                0x08 => {
                    i.src = Ea::dreg(dn);
                    i.dst = Ea::dreg(reg);
                }
                0x09 => {
                    i.src = Ea::areg(dn);
                    i.dst = Ea::areg(reg);
                }
                0x11 => {
                    i.src = Ea::dreg(dn);
                    i.dst = Ea::areg(reg);
                }
                _ => return false,
            }
            return true;
        }
    }
    i.src = Ea::dreg(dn);
    if line == 0xB {
        i.alu = A_EOR;
        return dec_ea_class(r, &mut i.dst, mode, reg, size, EA_DATA_ALT);
    }
    dec_ea_class(r, &mut i.dst, mode, reg, size, EA_MEM_ALT)
}

/// Line E: shifts and rotates; bit fields (68020) through the handler.
fn dec_line_e(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    let size = size_field(op);
    if size == 0 {
        if op & 0x0800 != 0 {
            // BFxxx, through the handler
            let ext = r.rd16();
            let mode = op_mode(op);
            let typ = (op >> 8) & 7;
            if mode == 0 {
                return call_handler(i, 1 << (op & 7) | 1 << ((ext >> 12) & 7));
            }
            // In memory: the 5 bytes a field may cover are checked first,
            // from the address and the offset (in Dn, or 0-31).
            let reads_only = matches!(typ, 0 | 1 | 3 | 5);
            let classes = if reads_only {
                EA_CONTROL
            } else {
                EA_CONTROL_ALT
            };
            if !dec_ea_class(r, &mut i.dst, mode, op_reg(op), 1, classes) {
                return false;
            }
            call_handler(i, 1 << ((ext >> 12) & 7));
            i.sub = 1;
            i.ext = ext as u16;
            i.write = !reads_only;
            i.size = 5;
            return true;
        }
        // <op> <ea>: a word in memory, by one
        i.kind = Kind::Shift;
        i.sub = ((op >> 9) & 3) as u8;
        i.left = op & 0x0100 != 0;
        i.count = 1;
        i.size = 2;
        return dec_ea_class(r, &mut i.dst, op_mode(op), op_reg(op), 2, EA_MEM_ALT);
    }
    if op & 0x0020 != 0 {
        // count in a register
        return call_handler(i, 1 << (op & 7));
    }
    let ccc = op_reg9(op);
    i.kind = Kind::Shift;
    i.sub = ((op >> 3) & 3) as u8;
    i.left = op & 0x0100 != 0;
    i.count = if ccc != 0 { ccc as u8 } else { 8 };
    i.size = size;
    i.dst = Ea::dreg(op_reg(op));
    true
}

/// Line F, coprocessor 1: the FPU instructions translated as calls of
/// fpu_jit_op() (Kind::Fpu), with their memory operand checked and
/// addressed first: FMOVE in and out (L S X W D B), the arithmetic most
/// code uses (FADD FSUB FMUL FDIV FSQRT FABS FNEG FCMP FTST, and the
/// 68040's single and double precision forms of them); and FBcc, by
/// fpu_jit_branch(). Nothing that the FPU of this model leaves to line F.
fn fpu_opmode(fpu: Fpu, m: u32) -> bool {
    const COMMON: [u32; 10] = [0x00, 0x04, 0x18, 0x1A, 0x20, 0x22, 0x23, 0x28, 0x38, 0x3A];
    const SD: [u32; 16] = [
        0x40, 0x41, 0x44, 0x45, 0x58, 0x5A, 0x5C, 0x5E, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68,
        0x6C,
    ];
    COMMON.contains(&m) || (fpu == Fpu::Internal && SD.contains(&m))
}

fn dec_line_f(r: &mut Reader, i: &mut Insn, op: u16) -> bool {
    const FMT_SIZE: [u32; 8] = [4, 4, 12, 12, 2, 8, 1, 12];
    let fpu = r.c.fpu;
    let mode = op_mode(op);
    let reg = op_reg(op);
    if fpu == Fpu::None {
        return false;
    }
    if op & 0xFF80 == 0xF280 {
        // FBcc
        if op & 0x20 != 0 {
            return false;
        }
        let base = i.pc.wrapping_add(2);
        let disp = if op & 0x40 != 0 {
            r.rd32()
        } else {
            sx16(r.rd16())
        };
        i.target = base.wrapping_add(disp);
        i.kind = Kind::Fbcc;
        i.cc = u32::from(op & 0x3F);
        return i.target & 1 == 0;
    }
    if op & 0xFFC0 != 0xF200 {
        return false;
    }
    let cmd = r.rd16();
    let opclass = cmd >> 13;
    let spec = (cmd >> 10) & 7;
    i.kind = Kind::Fpu;
    i.ext = cmd as u16;
    match opclass {
        0 => return op & 0x3F == 0 && fpu_opmode(fpu, cmd & 0x7F), // FPm to FPn
        2 | 3 => {}                                                // <ea> to FPn, FPn to <ea>
        _ => return false,
    }
    if spec == 3 || spec == 7 {
        return false; // packed, FMOVECR
    }
    if opclass == 2 && !fpu_opmode(fpu, cmd & 0x7F) {
        return false;
    }
    if opclass == 3 && cmd & 0x7F != 0 {
        return false; // the k-factor of packed
    }
    let size = FMT_SIZE[spec as usize];
    let shortf = size <= 4;
    i.size = size;
    if mode == 0 {
        // Dn: L S W B
        if !shortf {
            return false;
        }
        i.dst = Ea::dreg(reg);
        return true;
    }
    if mode == 1 || (mode == 7 && reg > if opclass == 2 { 4 } else { 1 }) {
        return false;
    }
    if mode == 7 && reg == 4 {
        // #imm: a byte in a word
        let mut k = 0;
        while k < size.max(2) {
            r.rd16();
            k += 2;
        }
        i.dst = Ea::imm(0);
        return true;
    }
    let ea_size = match size {
        1 => 1,
        2 => 2,
        _ => 4,
    };
    if !dec_ea(r, &mut i.dst, mode, reg, ea_size) {
        return false;
    }
    // (An)+ and -(An) step by the operand's size (dec_ea took 1, 2, 4; a
    // byte moves A7 by 2)
    if i.dst.step != 0 && size > 4 {
        i.dst.step = if i.dst.step < 0 {
            -(size as i8)
        } else {
            size as i8
        };
    }
    i.write = opclass == 3;
    true
}

// ---- Checks --------------------------------------------------------------------

/// A memory access at a constant address, checked now: it must be a plain
/// access, as the translated code makes no check for it (other than for
/// writes into translated code). The page map only changes through
/// relevo_map(), which drops every translation.
fn const_access_ok(c: &Ctx, addr: u32, size: u32, write: bool) -> bool {
    if size > 1 && addr & 1 != 0 && c.align_check {
        return false;
    }
    let a = addr & c.addr_mask;
    let kind = c.mem.kind(a);
    let ok = if write {
        kind == PAGE_RAM || kind == PAGE_RAM_CODE
    } else {
        direct_read(kind)
    };
    ok && in_page(a, size)
}

/// A constant access that the interpreter can do without a fault, through
/// the host's functions or a dropped ROM write.
fn const_access_host(c: &Ctx, addr: u32, size: u32) -> bool {
    if size > 1 && addr & 1 != 0 && c.align_check {
        return false;
    }
    (0..size).all(|i| c.mem.kind(addr.wrapping_add(i) & c.addr_mask) != PAGE_NONE)
}

/// An operand in memory that is read and/or written: a constant address is
/// checked now, any other is checked by the code (and may bail).
fn mem_operand(
    c: &Ctx,
    cycles: &mut u32,
    may_bail: &mut bool,
    e: &mut Ea,
    size: u32,
    write: bool,
) -> bool {
    if e.kind != EaKind::Mem {
        return true;
    }
    *cycles += if size == 4 { 8 } else { 4 };
    if e.mem_const() {
        // A write may still hit translated code.
        if write {
            *may_bail = true;
        }
        if const_access_ok(c, e.disp, size, write) {
            return true;
        }
        if !const_access_host(c, e.disp, size) {
            return false;
        }
        e.host = true;
        *may_bail = true;
        return true;
    }
    *may_bail = true;
    true
}

/// Decodes the instruction at `pc` (even, in a RAM or ROM page). Returns
/// None if the JIT does not translate it there. `rom` is cleared if any of
/// its words is in RAM.
pub fn decode(c: &Ctx, pc: u32, rom: &mut bool) -> Option<Insn> {
    let mut i = Insn {
        pc,
        ..Insn::default()
    };
    if pc & 1 != 0 {
        return None;
    }
    let mut r = Reader {
        c,
        pc,
        ok: true,
        rom,
    };
    let op = r.rd16() as u16;
    if !r.ok || !c.legal[usize::from(op)] {
        return None;
    }
    i.op = op;

    let ok = match op >> 12 {
        0x0 => dec_line0(&mut r, &mut i, op),
        0x1..=0x3 => dec_move(&mut r, &mut i, op),
        0x4 => dec_line4(&mut r, &mut i, op),
        0x5 => dec_line5(&mut r, &mut i, op),
        0x6 => dec_branch(&mut r, &mut i, op),
        0x7 => {
            i.kind = Kind::Moveq;
            i.size = 4;
            i.src = Ea::imm(sx8(u32::from(op)));
            i.dst = Ea::dreg(op_reg9(op));
            op & 0x0100 == 0
        }
        0x8 | 0x9 | 0xB | 0xC | 0xD => dec_alu_line(&mut r, &mut i, op),
        0xE => dec_line_e(&mut r, &mut i, op),
        0xF => dec_line_f(&mut r, &mut i, op),
        _ => false,
    };
    if !ok || !r.ok {
        return None;
    }
    i.len = r.pc.wrapping_sub(pc);
    if u64::from(pc & c.addr_mask) + u64::from(i.len) > u64::from(c.addr_mask) + 1 {
        return None;
    }
    if finish(c, &mut i) { Some(i) } else { None }
}

/// The registers, flags, checks and cycles of a decoded instruction.
fn finish(c: &Ctx, i: &mut Insn) -> bool {
    i.cycles = 2 * i.len;
    i.regs = i.src.regs() | i.dst.regs();
    let mut w = i.src.steps() | i.dst.steps();
    let a7: u16 = 1 << ra(7);
    let size = i.size;
    let mut ok = true;
    let mut cyc = i.cycles;
    let mut bail = i.may_bail;
    macro_rules! mem {
        ($e:ident, $size:expr, $write:expr) => {
            mem_operand(c, &mut cyc, &mut bail, &mut i.$e, $size, $write)
        };
    }
    match i.kind {
        Kind::Move => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
            ok = mem!(src, size, false) && mem!(dst, size, true);
        }
        Kind::Movea | Kind::Adda => {
            if i.kind == Kind::Adda && i.alu == A_CMP {
                i.flags_def = F_NZVC;
            } else {
                w |= i.dst.reg_of();
            }
            ok = mem!(src, size, false);
        }
        Kind::Moveq => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
        }
        Kind::Lea => {
            w |= i.dst.reg_of();
            if i.src.ind != 0 {
                bail = true;
            }
        }
        Kind::Pea => {
            i.regs |= a7;
            w |= a7;
            bail = true;
            cyc += 8;
        }
        Kind::Alu => {
            i.flags_def = if i.alu == A_ADD || i.alu == A_SUB {
                F_ALL
            } else {
                F_NZVC
            };
            if i.alu != A_CMP {
                w |= i.dst.reg_of();
            }
            let wr = i.alu != A_CMP;
            ok = mem!(src, size, false) && mem!(dst, size, wr);
            if i.dst.kind == EaKind::Mem && i.alu != A_CMP {
                cyc += if size == 4 { 8 } else { 4 };
            }
        }
        Kind::Cmpm => {
            i.flags_def = F_NZVC;
            ok = mem!(src, size, false) && mem!(dst, size, false);
        }
        Kind::Tst => {
            i.flags_def = F_NZVC;
            ok = mem!(src, size, false);
        }
        Kind::Clr => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
            ok = mem!(dst, size, true);
        }
        Kind::Unary => {
            if i.sub == U_NOT {
                i.flags_def = F_NZVC;
            } else {
                i.flags_def = F_ALL;
                if i.sub == U_NEGX {
                    i.flags_use = F_X | F_Z;
                }
            }
            w |= i.dst.reg_of();
            ok = mem!(dst, size, true);
        }
        Kind::Addx => {
            i.flags_def = F_ALL;
            i.flags_use = F_X | F_Z;
            w |= i.dst.reg_of();
            ok = mem!(src, size, false) && mem!(dst, size, true);
        }
        Kind::Shift => {
            i.flags_def = if i.sub == S_RO { F_NZVC } else { F_ALL };
            if i.sub == S_ROX {
                i.flags_use = F_X;
            }
            w |= i.dst.reg_of();
            ok = mem!(dst, size, true);
            cyc += 2 + 2 * u32::from(i.count);
        }
        Kind::Bit => {
            i.flags_def = F_Z;
            if i.sub != B_TST {
                w |= i.dst.reg_of();
            }
            let wr = i.sub != B_TST;
            ok = mem!(dst, size, wr);
            if i.dst.kind == EaKind::Imm && i.src.kind != EaKind::Dreg {
                ok = false;
            }
        }
        Kind::Mul => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
            ok = mem!(src, size, false);
            cyc += 38;
        }
        Kind::Div => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
            bail = true; // division by zero
            ok = mem!(src, size, false);
            cyc += 120;
        }
        Kind::Mull | Kind::Divl => {
            let rl = (i.ext >> 12) & 7;
            let rh = i.ext & 7;
            let mut m: u16 = 1 << rl;
            if i.ext & 0x0400 != 0 || i.kind == Kind::Divl {
                m |= 1 << rh;
            }
            if i.ext & 0x83F8 != 0 {
                return false; // reserved bits
            }
            i.regs |= m;
            w |= m;
            i.flags_def = F_NZVC;
            if i.kind == Kind::Divl {
                bail = true;
                if i.src.kind == EaKind::Imm && i.src.disp == 0 {
                    return false;
                }
            }
            ok = mem!(src, size, false);
            cyc += if i.kind == Kind::Divl { 70 } else { 40 };
        }
        Kind::Ext | Kind::Swap => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
        }
        Kind::Exg => {
            w |= i.src.reg_of() | i.dst.reg_of();
            cyc += 2;
        }
        Kind::Scc => {
            i.flags_use = cc_flags(i.cc);
            w |= i.dst.reg_of();
            ok = mem!(dst, 1, true);
        }
        Kind::Movem => {
            let n = i.mask.count_ones();
            let to_regs = i.sub != 0;
            if to_regs {
                w |= i.mask;
            }
            let e = if to_regs { i.src } else { i.dst };
            if e.kind == EaKind::Mem
                && e.step == 0
                && i.op & 0x38 == if to_regs { 0x18 } else { 0x20 }
            {
                w |= 1 << e.base; // (An)+ or -(An)
            }
            ok = if to_regs {
                mem!(src, n * size, false)
            } else {
                mem!(dst, n * size, true)
            };
            cyc += n * if size == 4 { 8 } else { 4 };
        }
        Kind::Link | Kind::Unlk => {
            i.regs |= a7;
            w |= a7 | i.dst.reg_of();
            bail = true;
            cyc += 12;
        }
        Kind::Tas => {
            i.flags_def = F_NZVC;
            w |= i.dst.reg_of();
            ok = mem!(dst, 1, true);
        }
        Kind::Chk => {
            // 68020 on: no flag changes without the trap (see op_chk)
            i.flags_def = if c.model >= Model::M68020 { 0 } else { F_NZVC };
            bail = true;
            ok = mem!(src, size, false);
            cyc += 6;
        }
        Kind::Cas => {
            let m: u16 = 1 << (i.ext & 7) | 1 << ((i.ext >> 6) & 7);
            i.regs |= m;
            w |= 1 << (i.ext & 7);
            i.flags_def = F_NZVC;
            ok = mem!(dst, size, true);
            cyc += 8;
        }
        Kind::Movep => {
            if i.sub == 0 {
                w |= i.src.reg_of();
            }
            bail = true;
            cyc += 4 * size;
        }
        Kind::Call => {
            i.flags_use = F_ALL;
            i.regs = 0;
            w = i.wregs;
            cyc = 0; // the handler counts them
            match i.sub {
                2 => {
                    i.regs = w;
                    bail = true;
                }
                3 => {
                    i.regs = i.dst.regs();
                    w = i.dst.steps();
                    if i.dst.mem_const() {
                        ok = const_access_ok(c, i.dst.disp, 1, true);
                    }
                    bail = true;
                }
                1 => {
                    // the address and the offset register, for the check
                    i.regs = i.dst.regs();
                    if i.ext & 0x0800 != 0 {
                        i.regs |= 1 << ((i.ext >> 6) & 7);
                    }
                    bail = true;
                    if i.dst.mem_const() && i.ext & 0x0800 == 0 {
                        let off = (u32::from(i.ext) >> 6 & 31) >> 3;
                        ok = const_access_ok(c, i.dst.disp.wrapping_add(off), 5, i.write);
                    }
                }
                _ => {}
            }
        }
        Kind::Fpu => {
            bail = true; // an FPU exception to take
            if i.dst.kind == EaKind::Dreg && i.ext >> 13 == 3 {
                w |= i.dst.reg_of();
            }
            let wr = i.write;
            ok = mem!(dst, size, wr);
            cyc += 40;
        }
        Kind::Fbcc => {
            bail = true; // an FPU exception to take
            i.ends_block = true;
            cyc += 6;
        }
        Kind::Bcc => {
            i.flags_use = cc_flags(i.cc);
            i.ends_block = true;
            cyc += 6;
        }
        Kind::Dbcc => {
            i.flags_use = cc_flags(i.cc);
            i.ends_block = true;
            w |= i.dst.reg_of();
            cyc += 6;
        }
        Kind::Bsr | Kind::Jsr | Kind::Rts => {
            i.regs |= a7;
            w |= a7;
            bail = true;
            i.ends_block = true;
            cyc += 16;
            if i.kind == Kind::Jsr && i.src.mem_const() && i.src.disp & 1 != 0 {
                ok = false;
            }
        }
        Kind::Jmp => {
            i.ends_block = true;
            cyc += 6;
            if i.src.mem_const() {
                ok = i.src.disp & 1 == 0;
            } else {
                bail = true; // odd target
            }
        }
        Kind::Nop => {}
    }
    i.cycles = cyc;
    i.may_bail = bail;
    i.wregs = w;
    ok
}

/// The table of a condition: bit f is whether condition `cc` holds for the
/// CCR bits N Z V C = f.
pub fn cond_table(cc: u32) -> u32 {
    (0..16u16)
        .filter(|&f| cond_true(f, cc))
        .fold(0, |t, f| t | 1 << f)
}
