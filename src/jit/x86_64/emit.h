/* SPDX-License-Identifier: MPL-2.0
 *
 * A small x86-64 instruction encoder (Intel SDM volume 2): just what the
 * translator uses. Registers are numbered as in the encoding (RAX 0 ...
 * R15 15). Operand sizes are 1, 2, 4 or 8 bytes.
 */
#ifndef RELEVO_JIT_X86_EMIT_H
#define RELEVO_JIT_X86_EMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
};

/* Condition codes (the low nibble of Jcc/SETcc/CMOVcc). */
enum {
    XC_O = 0, XC_NO, XC_B, XC_AE, XC_E, XC_NE, XC_BE, XC_A,
    XC_S, XC_NS, XC_P, XC_NP, XC_L, XC_GE, XC_LE, XC_G
};

/* ALU operations: the /digit of 80/81/83 and the row of 00-3F. */
enum { XA_ADD = 0, XA_OR = 1, XA_AND = 4, XA_SUB = 5, XA_XOR = 6, XA_CMP = 7 };

typedef struct xbuf {
    uint8_t *p;         /* where the bytes go (the RW view) */
    size_t pos, cap;
    bool overflow;
} xbuf;

/* A memory operand: [base + index << scale + disp]. -1: none. */
typedef struct xmem {
    int base, index, scale;
    int32_t disp;
} xmem;

static inline xmem xm(int base, int32_t disp)
{
    xmem m = { base, -1, 0, disp };
    return m;
}

static inline xmem xmi(int base, int index, int scale, int32_t disp)
{
    xmem m = { base, index, scale, disp };
    return m;
}

static inline void x_b(xbuf *b, uint8_t v)
{
    if (b->pos < b->cap)
        b->p[b->pos] = v;
    else
        b->overflow = true;
    b->pos++;
}

static inline void x_w(xbuf *b, uint16_t v)
{
    x_b(b, (uint8_t)v);
    x_b(b, (uint8_t)(v >> 8));
}

static inline void x_d(xbuf *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        x_b(b, (uint8_t)(v >> (8 * i)));
}

static inline void x_patch32(xbuf *b, size_t at, uint32_t v)
{
    if (at + 4 <= b->cap)
        for (int i = 0; i < 4; i++)
            b->p[at + i] = (uint8_t)(v >> (8 * i));
}

static inline bool fits8(int32_t v)
{
    return v >= -128 && v <= 127;
}

/* Prefixes: 66 for 16 bits, REX (W for 64 bits; R, X, B for the high
 * registers; a bare REX when `force_rex`, for SPL/BPL/SIL/DIL as byte
 * registers). */
static inline void x_prefix(xbuf *b, int size, int reg, int index, int base,
                            bool force_rex)
{
    if (size == 2)
        x_b(b, 0x66);
    unsigned rex = 0;
    if (size == 8)
        rex |= 8;
    if (reg >= 8)
        rex |= 4;
    if (index >= 8)
        rex |= 2;
    if (base >= 8)
        rex |= 1;
    if (rex || force_rex)
        x_b(b, (uint8_t)(0x40 | rex));
}

static inline bool x_low_byte_needs_rex(int r)
{
    return r >= 4 && r < 8;
}

static inline void x_opc(xbuf *b, const uint8_t *opc, int n)
{
    for (int i = 0; i < n; i++)
        x_b(b, opc[i]);
}

/* opcode with a register operand in r/m. `byte_rm`: the r/m (and reg)
 * operand is a byte register. */
static inline void x_rr(xbuf *b, int size, const uint8_t *opc, int n,
                        int reg, int rm, bool byte_regs)
{
    x_prefix(b, size, reg, -1, rm,
             byte_regs && (x_low_byte_needs_rex(reg)
                           || x_low_byte_needs_rex(rm)));
    x_opc(b, opc, n);
    x_b(b, (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

static inline void x_modrm_mem(xbuf *b, int reg, xmem m)
{
    reg &= 7;
    if (m.base < 0) {
        /* [index*s + disp32] or [disp32]: SIB with no base */
        x_b(b, (uint8_t)(0x04 | reg << 3));
        int idx = m.index < 0 ? 4 : (m.index & 7);
        x_b(b, (uint8_t)((m.index < 0 ? 0 : m.scale) << 6 | idx << 3 | 5));
        x_d(b, (uint32_t)m.disp);
        return;
    }
    int mod;
    if (m.disp == 0 && (m.base & 7) != 5)
        mod = 0;
    else if (fits8(m.disp))
        mod = 1;
    else
        mod = 2;
    if (m.index >= 0 || (m.base & 7) == 4) {
        int idx = m.index < 0 ? 4 : (m.index & 7);
        int sc = m.index < 0 ? 0 : m.scale;
        x_b(b, (uint8_t)(mod << 6 | reg << 3 | 4));
        x_b(b, (uint8_t)(sc << 6 | idx << 3 | (m.base & 7)));
    } else {
        x_b(b, (uint8_t)(mod << 6 | reg << 3 | (m.base & 7)));
    }
    if (mod == 1)
        x_b(b, (uint8_t)(int8_t)m.disp);
    else if (mod == 2)
        x_d(b, (uint32_t)m.disp);
}

static inline void x_rm(xbuf *b, int size, const uint8_t *opc, int n,
                        int reg, xmem m, bool byte_reg)
{
    x_prefix(b, size, reg, m.index, m.base < 0 ? -1 : m.base,
             byte_reg && x_low_byte_needs_rex(reg));
    x_opc(b, opc, n);
    x_modrm_mem(b, reg, m);
}

/* ---- Moves ------------------------------------------------------------ */

static inline void x_mov_rr(xbuf *b, int size, int dst, int src)
{
    uint8_t o = size == 1 ? 0x88 : 0x89;
    x_rr(b, size, &o, 1, src, dst, size == 1);
}

static inline void x_mov_ri(xbuf *b, int dst, uint32_t imm)  /* 32-bit */
{
    x_prefix(b, 4, -1, -1, dst, false);
    x_b(b, (uint8_t)(0xB8 + (dst & 7)));
    x_d(b, imm);
}

/* MOV r/m (size 1, 2 or 4), imm: only the low `size` bytes change. */
static inline void x_mov_ri_sz(xbuf *b, int size, int dst, uint32_t imm)
{
    if (size == 4) {
        x_mov_ri(b, dst, imm);
        return;
    }
    uint8_t o = size == 1 ? 0xC6 : 0xC7;
    x_rr(b, size, &o, 1, 0, dst, size == 1);
    if (size == 1)
        x_b(b, (uint8_t)imm);
    else
        x_w(b, (uint16_t)imm);
}

static inline void x_load(xbuf *b, int size, int dst, xmem m)
{
    uint8_t o = size == 1 ? 0x8A : 0x8B;
    x_rm(b, size, &o, 1, dst, m, size == 1);
}

static inline void x_store(xbuf *b, int size, xmem m, int src)
{
    uint8_t o = size == 1 ? 0x88 : 0x89;
    x_rm(b, size, &o, 1, src, m, size == 1);
}

static inline void x_store_imm(xbuf *b, int size, xmem m, uint32_t imm)
{
    uint8_t o = size == 1 ? 0xC6 : 0xC7;
    x_rm(b, size, &o, 1, 0, m, false);
    if (size == 1)
        x_b(b, (uint8_t)imm);
    else if (size == 2)
        x_w(b, (uint16_t)imm);
    else
        x_d(b, imm);
}

/* MOVZX/MOVSX r32, r/m8 or r/m16 */
static inline void x_movzx_load(xbuf *b, int from, int dst, xmem m)
{
    uint8_t o[2] = { 0x0F, from == 1 ? 0xB6 : 0xB7 };
    x_rm(b, 4, o, 2, dst, m, false);
}

static inline void x_movzx_rr(xbuf *b, int from, int dst, int src)
{
    uint8_t o[2] = { 0x0F, from == 1 ? 0xB6 : 0xB7 };
    x_rr(b, 4, o, 2, dst, src, from == 1);
}

static inline void x_movsx_rr(xbuf *b, int from, int dst, int src)
{
    uint8_t o[2] = { 0x0F, from == 1 ? 0xBE : 0xBF };
    x_rr(b, 4, o, 2, dst, src, from == 1);
}

/* MOVBE r16/r32, m and m, r16/r32 */
static inline void x_movbe_load(xbuf *b, int size, int dst, xmem m)
{
    static const uint8_t o[3] = { 0x0F, 0x38, 0xF0 };
    x_rm(b, size, o, 3, dst, m, false);
}

static inline void x_movbe_store(xbuf *b, int size, xmem m, int src)
{
    static const uint8_t o[3] = { 0x0F, 0x38, 0xF1 };
    x_rm(b, size, o, 3, src, m, false);
}

static inline void x_bswap32(xbuf *b, int r)
{
    x_prefix(b, 4, -1, -1, r, false);
    x_b(b, 0x0F);
    x_b(b, (uint8_t)(0xC8 + (r & 7)));
}

/* ROL r16, 8: swaps the bytes of a word (changes CF and OF). */
static inline void x_rol16_8(xbuf *b, int r)
{
    uint8_t o = 0xC1;
    x_rr(b, 2, &o, 1, 0, r, false);
    x_b(b, 8);
}

/* XCHG CL, CH: swaps the bytes of CX without touching the flags. */
static inline void x_xchg_cl_ch(xbuf *b)
{
    x_b(b, 0x86);
    x_b(b, 0xE9);
}

static inline void x_lea(xbuf *b, int dst, xmem m)   /* 32-bit result */
{
    uint8_t o = 0x8D;
    x_rm(b, 4, &o, 1, dst, m, false);
}

/* ---- Arithmetic ------------------------------------------------------- */

static inline void x_alu_rr(xbuf *b, int op, int size, int dst, int src)
{
    uint8_t o = (uint8_t)(op << 3 | (size == 1 ? 0 : 1));
    x_rr(b, size, &o, 1, src, dst, size == 1);
}

static inline void x_alu_ri(xbuf *b, int op, int size, int dst, uint32_t imm)
{
    if (size == 1) {
        uint8_t o = 0x80;
        x_rr(b, 1, &o, 1, op, dst, true);
        x_b(b, (uint8_t)imm);
        return;
    }
    int32_t s = size == 2 ? (int16_t)imm : (int32_t)imm;
    uint8_t o = fits8(s) ? 0x83 : 0x81;
    x_rr(b, size, &o, 1, op, dst, false);
    if (o == 0x83)
        x_b(b, (uint8_t)(int8_t)s);
    else if (size == 2)
        x_w(b, (uint16_t)imm);
    else
        x_d(b, imm);
}

static inline void x_alu_mi(xbuf *b, int op, int size, xmem m, int32_t imm)
{
    if (size == 1) {
        uint8_t o = 0x80;
        x_rm(b, 1, &o, 1, op, m, false);
        x_b(b, (uint8_t)imm);
        return;
    }
    uint8_t o = fits8(imm) ? 0x83 : 0x81;
    x_rm(b, size, &o, 1, op, m, false);
    if (o == 0x83)
        x_b(b, (uint8_t)(int8_t)imm);
    else if (size == 2)
        x_w(b, (uint16_t)imm);
    else
        x_d(b, (uint32_t)imm);
}

static inline void x_test_rr(xbuf *b, int size, int a, int c)
{
    uint8_t o = size == 1 ? 0x84 : 0x85;
    x_rr(b, size, &o, 1, c, a, size == 1);
}

static inline void x_test_ri(xbuf *b, int size, int r, uint32_t imm)
{
    uint8_t o = size == 1 ? 0xF6 : 0xF7;
    x_rr(b, size, &o, 1, 0, r, size == 1);
    if (size == 1)
        x_b(b, (uint8_t)imm);
    else if (size == 2)
        x_w(b, (uint16_t)imm);
    else
        x_d(b, imm);
}

/* SHL (4), SHR (5) r32, imm */
static inline void x_shift_ri(xbuf *b, int op, int r, int n)
{
    uint8_t o = 0xC1;
    x_rr(b, 4, &o, 1, op, r, false);
    x_b(b, (uint8_t)n);
}

/* BT r32, r32: CF = bit `bit` of `base`. */
static inline void x_bt_rr(xbuf *b, int base, int bit)
{
    static const uint8_t o[2] = { 0x0F, 0xA3 };
    x_rr(b, 4, o, 2, bit, base, false);
}

static inline void x_pushfq(xbuf *b) { x_b(b, 0x9C); }

static inline void x_push(xbuf *b, int r)
{
    if (r >= 8)
        x_b(b, 0x41);
    x_b(b, (uint8_t)(0x50 + (r & 7)));
}

static inline void x_pop(xbuf *b, int r)
{
    if (r >= 8)
        x_b(b, 0x41);
    x_b(b, (uint8_t)(0x58 + (r & 7)));
}

static inline void x_ret(xbuf *b) { x_b(b, 0xC3); }

/* SUB/ADD RSP, imm8 */
static inline void x_rsp_adjust(xbuf *b, int32_t v)
{
    x_alu_ri(b, v < 0 ? XA_SUB : XA_ADD, 8, RSP, (uint32_t)(v < 0 ? -v : v));
}

/* ---- Jumps ------------------------------------------------------------ */

/* Jcc rel32 / JMP rel32 with a target to fill in later: returns the
 * position of the rel32 field. */
static inline size_t x_jcc32(xbuf *b, int cc)
{
    x_b(b, 0x0F);
    x_b(b, (uint8_t)(0x80 + cc));
    size_t at = b->pos;
    x_d(b, 0);
    return at;
}

static inline size_t x_jmp32(xbuf *b)
{
    x_b(b, 0xE9);
    size_t at = b->pos;
    x_d(b, 0);
    return at;
}

/* Points the rel32 field at `at` to position `to` (relative to the start
 * of the buffer; may be negative). */
static inline void x_link(xbuf *b, size_t at, int64_t to)
{
    x_patch32(b, at, (uint32_t)(to - (int64_t)(at + 4)));
}

/* JMP r64 */
static inline void x_jmp_r(xbuf *b, int r)
{
    uint8_t o = 0xFF;
    x_rr(b, 4, &o, 1, 4, r, false);
}

static inline void x_cmov(xbuf *b, int cc, int dst, int src)  /* 32-bit */
{
    uint8_t o[2] = { 0x0F, (uint8_t)(0x40 + cc) };
    x_rr(b, 4, o, 2, dst, src, false);
}

#endif
