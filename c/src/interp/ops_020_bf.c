/* SPDX-License-Identifier: MPL-2.0
 *
 * Bit fields of the 68020 and later (M68000PRM 4, MC68020UM): BFTST,
 * BFEXTU, BFEXTS, BFCHG, BFCLR, BFSET, BFFFO and BFINS.
 *
 * Extension word (it comes before the operand's own words):
 *
 *   bits 14-12  Dn of BFEXTU, BFEXTS, BFFFO and BFINS
 *   bit 11      Do: the offset is in Dn (bits 8-6), else bits 10-6
 *   bit 5       Dw: the width is in Dn (bits 2-0), else bits 4-0
 *
 * The width is taken modulo 32, 0 meaning 32. Bit 0 of a field is its most
 * significant bit. In a data register the offset is taken modulo 32 and
 * the field wraps from bit 0 round to bit 31. In memory the offset is
 * signed (-2^31 to 2^31-1 from Dn, 0-31 immediate) and counts from bit 7
 * of the byte at <ea>; the field may cover up to 5 bytes.
 *
 * Flags, from the field before the change (BFINS: from the value
 * inserted): N its top bit, Z if it is all zeros, V and C cleared, X kept.
 */
#include "ops_020.h"

enum bf_op { BF_TST, BF_EXTU, BF_CHG, BF_EXTS, BF_CLR, BF_FFO, BF_SET, BF_INS };

/* Low `width` (1-32) bits set. */
static uint32_t bf_mask(unsigned width)
{
    return width == 32 ? 0xFFFFFFFFu : (1u << width) - 1;
}

static uint32_t rol32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? v << n | v >> (32 - n) : v;
}

/* Where the field is and its contents, right-aligned. */
typedef struct bitfield {
    ea_t e;
    int32_t offset;
    unsigned width;
    uint32_t addr;      /* memory: first byte */
    unsigned bytes;     /* memory: bytes covered */
    unsigned shift;     /* memory: bits below the field in those bytes */
    uint64_t window;    /* memory: those bytes */
    uint32_t value;
} bitfield;

static void bf_read(relevo_cpu *c, uint16_t op, uint32_t ext, bitfield *bf)
{
    bf->offset = (ext & 0x0800) ? (int32_t)c->r.d[(ext >> 6) & 7]
                                : (int32_t)((ext >> 6) & 31);
    unsigned w = (ext & 0x0020) ? c->r.d[ext & 7] & 31 : ext & 31;
    bf->width = w == 0 ? 32 : w;
    ea_decode_op(c, &bf->e, op, SZ_B);

    if (bf->e.kind == EA_DREG) {
        uint32_t rot = rol32(c->r.d[bf->e.reg], (uint32_t)bf->offset);
        bf->value = rot >> (32 - bf->width);
        return;
    }
    /* byte offset: the signed offset divided by 8, rounded down */
    uint32_t off = (uint32_t)bf->offset;
    uint32_t byte_off = off >> 3;
    if (bf->offset < 0)
        byte_off |= 0xE0000000u;
    unsigned bit = off & 7;
    bf->addr = bf->e.addr + byte_off;
    bf->bytes = (bit + bf->width + 7) / 8;
    bf->shift = 8 * bf->bytes - bit - bf->width;
    bf->window = 0;
    for (unsigned i = 0; i < bf->bytes; i++)
        bf->window = bf->window << 8
                   | cpu_read8(c, bf->addr + i, bf->e.acc);
    bf->value = (uint32_t)(bf->window >> bf->shift) & bf_mask(bf->width);
}

static void bf_write(relevo_cpu *c, bitfield *bf, uint32_t v)
{
    uint32_t m = bf_mask(bf->width);
    v &= m;
    if (bf->e.kind == EA_DREG) {
        unsigned rot = (uint32_t)bf->offset & 31;
        uint32_t d = rol32(c->r.d[bf->e.reg], rot);
        unsigned low = 32 - bf->width;
        d = (d & ~(m << low)) | v << low;
        c->r.d[bf->e.reg] = rol32(d, 32 - rot);
        return;
    }
    uint64_t w = (bf->window & ~((uint64_t)m << bf->shift))
               | (uint64_t)v << bf->shift;
    for (unsigned i = 0; i < bf->bytes; i++)
        cpu_write8(c, bf->addr + i,
                   (uint32_t)(w >> (8 * (bf->bytes - 1 - i))) & 0xFF);
}

static void bf_flags(relevo_cpu *c, uint32_t v, unsigned width)
{
    unsigned f = 0;
    if ((v & bf_mask(width)) == 0)
        f |= CCR_Z;
    if (v & (1u << (width - 1)))
        f |= CCR_N;
    set_ccr_bits(c, CCR_N | CCR_Z | CCR_V | CCR_C, f);
}

static void bitfield_op(relevo_cpu *c, uint16_t op, enum bf_op kind)
{
    uint32_t ext = fetch16(c);
    unsigned dn = (ext >> 12) & 7;
    bitfield bf;
    bf_read(c, op, ext, &bf);
    uint32_t v = bf.value, m = bf_mask(bf.width);
    cycles(c, bf.e.kind == EA_DREG ? 4 : 8);

    if (kind == BF_INS) {
        uint32_t ins = c->r.d[dn] & m;
        bf_flags(c, ins, bf.width);
        bf_write(c, &bf, ins);
        return;
    }
    bf_flags(c, v, bf.width);
    switch (kind) {
    case BF_EXTU:
        c->r.d[dn] = v;
        break;
    case BF_EXTS:
        c->r.d[dn] = (v & (1u << (bf.width - 1))) ? v | ~m : v;
        break;
    case BF_FFO: {
        /* offset of the first 1 from the top of the field; offset + width
         * when there is none */
        unsigned i = 0;
        while (i < bf.width && !(v & (1u << (bf.width - 1 - i))))
            i++;
        c->r.d[dn] = (uint32_t)bf.offset + i;
        break;
    }
    case BF_CHG:
        bf_write(c, &bf, ~v);
        break;
    case BF_CLR:
        bf_write(c, &bf, 0);
        break;
    case BF_SET:
        bf_write(c, &bf, m);
        break;
    default:    /* BF_TST */
        break;
    }
}

static void op_bftst(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_TST); }
static void op_bfextu(relevo_cpu *c, uint16_t op) { bitfield_op(c, op, BF_EXTU); }
static void op_bfchg(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_CHG); }
static void op_bfexts(relevo_cpu *c, uint16_t op) { bitfield_op(c, op, BF_EXTS); }
static void op_bfclr(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_CLR); }
static void op_bfffo(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_FFO); }
static void op_bfset(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_SET); }
static void op_bfins(relevo_cpu *c, uint16_t op)  { bitfield_op(c, op, BF_INS); }

void ops_020_bf_init(op_table *tb)
{
    /* 1110 1ttt 11 <ea>: the ones that only read take control modes, the
     * others control alterable ones; all take Dn. */
    static const relevo_handler h[8] = {
        op_bftst, op_bfextu, op_bfchg, op_bfexts,
        op_bfclr, op_bfffo, op_bfset, op_bfins
    };
    for (uint16_t t = 0; t < 8; t++) {
        bool reads_only = t == BF_TST || t == BF_EXTU || t == BF_EXTS
                       || t == BF_FFO;
        unsigned classes = EAB_DREG
                         | (reads_only ? EA_CONTROL : EA_CONTROL_ALT);
        table_fill(tb, (uint16_t)(0xE8C0 | t << 8), 0x003F, classes, h[t]);
    }
}
