/* SPDX-License-Identifier: MPL-2.0
 *
 * Shifts and rotates, bit operations, BCD, multiply and divide (task R2c):
 * ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR, BTST, BCHG, BCLR, BSET, ABCD,
 * SBCD, NBCD, MULU, MULS, DIVU and DIVS.
 *
 * Not written yet: these opcodes still go to the illegal instruction.
 */
#include "interp.h"

void ops_bits_init(op_table *tb)
{
    (void)tb;
}
