/* SPDX-License-Identifier: MPL-2.0
 *
 * The interpreter's opcode table: one handler per 16-bit opcode word, filled
 * for the configured model. Opcodes the model does not execute go to the
 * illegal instruction, line A or line F handlers.
 */
#include "interp.h"

void interp_fill_table(relevo_handler *t, relevo_model model, relevo_fpu fpu)
{
    (void)model;
    (void)fpu;

    for (unsigned op = 0; op < 0x10000; op++)
        t[op] = op_illegal;
    for (unsigned op = 0xA000; op <= 0xAFFF; op++)
        t[op] = op_line_a;
    for (unsigned op = 0xF000; op <= 0xFFFF; op++)
        t[op] = op_line_f;

    /* All models */
    t[0x4E71] = op_nop;
    t[0x4E75] = op_rts;
}
