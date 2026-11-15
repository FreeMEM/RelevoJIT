/* SPDX-License-Identifier: MPL-2.0
 *
 * The caches of the 68020 to 68060, as far as software can see them: the
 * CACR register and the instructions that flush or invalidate. The engines
 * keep no copy of the 68k's caches; what matters is that code which the
 * 68k invalidates is also dropped by the JIT (relevo_invalidate()).
 *
 * Not part of the API.
 */
#ifndef RELEVO_CACHE_H
#define RELEVO_CACHE_H

#include "cpu.h"

/* CACR bits that hold a value on each model; the rest read as 0. */
uint32_t cache_cacr_mask(relevo_model model);

/* A write to CACR (MOVEC): keeps the bits the model has and acts on the
 * write-only ones of the 68020 and 68030 (C clears the instruction cache,
 * CE the entry that CAAR points at; CD and CED clear the data cache, which
 * needs nothing). */
void cache_write_cacr(relevo_cpu *cpu, uint32_t value);

/* Instruction cache invalidation: a 16-byte line, a page (8 KB, the larger
 * of the 68040's page sizes) or everything. */
void cache_invalidate_line(relevo_cpu *cpu, uint32_t address);
void cache_invalidate_page(relevo_cpu *cpu, uint32_t address);
void cache_invalidate_all(relevo_cpu *cpu);

#endif /* RELEVO_CACHE_H */
