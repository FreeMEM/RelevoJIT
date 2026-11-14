/* SPDX-License-Identifier: MPL-2.0
 *
 * 68040 and 68060: MOVE16, CINV and CPUSH (MC68040UM, M68000PRM 4 and 6).
 * The CACR register itself is in src/cache.c, for MOVEC.
 */
#include "../cache.h"
#include "ops_020.h"

/* Copies the 16-byte line holding `src` to the one holding `dst`. */
static void move16_line(relevo_cpu *c, uint32_t src, uint32_t dst)
{
    uint32_t line[4];
    src &= ~15u;
    dst &= ~15u;
    for (unsigned i = 0; i < 4; i++)
        line[i] = cpu_read32(c, src + 4 * i, ACC_DATA);
    for (unsigned i = 0; i < 4; i++)
        cpu_write32(c, dst + 4 * i, line[i]);
    cycles(c, 4);
}

/* MOVE16 (Ax)+,(Ay)+: Ay in bits 14-12 of the extension word. Both
 * registers go up by 16 (the same register twice, if Ax = Ay). */
static void op_move16_postinc(relevo_cpu *c, uint16_t op)
{
    uint32_t ext = fetch16(c);
    unsigned ax = op & 7, ay = (ext >> 12) & 7;
    move16_line(c, c->r.a[ax], c->r.a[ay]);
    c->r.a[ax] += 16;
    c->r.a[ay] += 16;
}

/* MOVE16 with an absolute address; bits 4-3: 00 (Ay)+,(xxx).L;
 * 01 (xxx).L,(Ay)+; 10 (Ay),(xxx).L; 11 (xxx).L,(Ay). */
static void op_move16_abs(relevo_cpu *c, uint16_t op)
{
    uint32_t abs = fetch32(c);
    unsigned ay = op & 7, mode = (op >> 3) & 3;
    if (mode & 1)
        move16_line(c, abs, c->r.a[ay]);
    else
        move16_line(c, c->r.a[ay], abs);
    if (!(mode & 2))
        c->r.a[ay] += 16;
}

/* CINV and CPUSH (privileged): 1111 0100 cc p ss rrr, caches in bits 7-6
 * (bit 6 data, bit 7 instruction), CPUSH in bit 5, scope in bits 4-3 (01
 * line, 10 page, 11 all; An holds the physical address). The data cache
 * needs nothing (memory is always up to date); code in the instruction
 * cache is dropped from the JIT. */
static void op_cache(relevo_cpu *c, uint16_t op)
{
    if (!supervisor_or_trap(c))
        return;
    cycles(c, 4);
    if (!(op & 0x0080))
        return;
    uint32_t addr = c->r.a[op & 7];
    switch ((op >> 3) & 3) {
    case 1:
        cache_invalidate_line(c, addr);
        break;
    case 2:
        cache_invalidate_page(c, addr);
        break;
    default:
        cache_invalidate_all(c);
        break;
    }
}

void ops_020_cache_init(op_table *tb)
{
    if (tb->model < RELEVO_68040)
        return;
    table_fill(tb, 0xF620, 0x0007, 0, op_move16_postinc);
    table_fill(tb, 0xF600, 0x001F, 0, op_move16_abs);
    /* scope 00 is not an instruction: it stays line F */
    for (uint16_t scope = 1; scope < 4; scope++)
        table_fill(tb, (uint16_t)(0xF400 | scope << 3), 0x00E7, 0, op_cache);
}
