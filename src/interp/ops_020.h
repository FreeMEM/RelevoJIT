/* SPDX-License-Identifier: MPL-2.0
 *
 * The files of the "020" group (task R3b), the instructions the 68020 and
 * later add: ops_020.c (multiply and divide long, CHK.L, CHK2, CMP2, CAS,
 * CAS2, TRAPcc, PACK, UNPK, EXTB.L, LINK.L and the new modes of CMPI and
 * TST), ops_020_bf.c (bit fields) and ops_020_cache.c (MOVE16, CINV and
 * CPUSH of the 68040).
 *
 * Cycles: there is no exact reference for these models. Bus accesses count
 * themselves as on the 68000; the handlers add rough internal times.
 */
#ifndef RELEVO_OPS_020_H
#define RELEVO_OPS_020_H

#include "interp.h"

void ops_020_bf_init(op_table *tb);

/* The trap and flags of CHK.W and CHK.L on the 68020+ (ops_020.c). */
void chk_020(relevo_cpu *cpu, int32_t value, int32_t bound);
void ops_020_cache_init(op_table *tb);

#endif /* RELEVO_OPS_020_H */
