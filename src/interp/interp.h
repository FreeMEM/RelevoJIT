/* SPDX-License-Identifier: MPL-2.0
 *
 * Interpreter handlers.
 */
#ifndef RELEVO_INTERP_H
#define RELEVO_INTERP_H

#include "../cpu.h"

/* ops_control.c */
void op_illegal(relevo_cpu *cpu, uint16_t opcode);
void op_line_a(relevo_cpu *cpu, uint16_t opcode);
void op_line_f(relevo_cpu *cpu, uint16_t opcode);
void op_nop(relevo_cpu *cpu, uint16_t opcode);
void op_rts(relevo_cpu *cpu, uint16_t opcode);

#endif /* RELEVO_INTERP_H */
