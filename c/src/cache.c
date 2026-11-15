/* SPDX-License-Identifier: MPL-2.0
 *
 * CACR and cache invalidation (see cache.h). The CACR layouts are those of
 * MC68020UM 6.3, MC68030UM 6.4, MC68040UM 4.1 and MC68060UM 5.3.
 */
#include "cache.h"

/* 68020: E (0) and F (1); C (3) and CE (2) are write-only. */
#define CACR_020        0x00000003u
/* 68030: WA (13), DBE (12), FD (9), ED (8), IBE (4), FI (1), EI (0); CD
 * (11), CED (10), C (3) and CE (2) are write-only. */
#define CACR_030        0x00003313u
/* 68040: DE (31), IE (15). */
#define CACR_040        0x80008000u
/* 68060: EDC (31), NAD (30), ESB (29), DPI (28), FOC (27), EBC (23), EIC
 * (15), NAI (14), FIC (13); CABC (22) and CUBC (21) are write-only. */
#define CACR_060        0xF880E000u

#define CACR_C          0x00000008u     /* 68020/030: clear the I-cache */
#define CACR_CE         0x00000004u     /* 68020/030: clear one entry */

uint32_t cache_cacr_mask(relevo_model model)
{
    switch (model) {
    case RELEVO_68020: return CACR_020;
    case RELEVO_68030: return CACR_030;
    case RELEVO_68040: return CACR_040;
    case RELEVO_68060: return CACR_060;
    default:           return 0;
    }
}

void cache_write_cacr(relevo_cpu *c, uint32_t v)
{
    if (c->model == RELEVO_68020 || c->model == RELEVO_68030) {
        if (v & CACR_C)
            cache_invalidate_all(c);
        else if (v & CACR_CE)
            cache_invalidate_line(c, c->r.caar);
    }
    c->r.cacr = v & cache_cacr_mask(c->model);
}

void cache_invalidate_line(relevo_cpu *c, uint32_t address)
{
    relevo_invalidate(c, address & ~15u, 16);
}

void cache_invalidate_page(relevo_cpu *c, uint32_t address)
{
    relevo_invalidate(c, address & ~0x1FFFu, 0x2000);
}

void cache_invalidate_all(relevo_cpu *c)
{
    /* The size is 32 bits: the whole space takes two halves. */
    relevo_invalidate(c, 0, 0x80000000u);
    relevo_invalidate(c, 0x80000000u, 0x80000000u);
}
