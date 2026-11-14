/* SPDX-License-Identifier: MPL-2.0
 *
 * The interpreter's opcode table: one handler per 16-bit opcode word, filled
 * for the configured model. Opcodes the model does not execute go to the
 * illegal instruction, line A or line F handlers.
 */
#include "interp.h"

bool ea_valid(unsigned mode, unsigned reg, unsigned classes)
{
    unsigned slot = mode < 7 ? mode : 7 + reg;
    if (mode == 7 && reg > 4)
        return false;
    return (classes >> slot) & 1;
}

static bool is_default(relevo_handler h)
{
    return h == op_illegal || h == op_line_a || h == op_line_f;
}

void table_set(op_table *tb, uint16_t op, relevo_handler h)
{
    if (!is_default(tb->t[op]))
        tb->conflicts++;
    tb->t[op] = h;
}

void table_fill(op_table *tb, uint16_t base, uint16_t free,
                unsigned ea_classes, relevo_handler h)
{
    /* Walks every subset s of `free`. */
    uint16_t s = 0;
    do {
        uint16_t op = (uint16_t)(base | s);
        if (ea_classes == 0 || ea_valid((op >> 3) & 7, op & 7, ea_classes))
            table_set(tb, op, h);
        s = (uint16_t)((s - free) & free);
    } while (s != 0);
}

unsigned interp_fill_table(relevo_handler *t, relevo_model model,
                           relevo_fpu fpu)
{
    op_table tb = { t, model, fpu, 0 };

    for (unsigned op = 0; op < 0x10000; op++)
        t[op] = op_illegal;
    for (unsigned op = 0xA000; op <= 0xAFFF; op++)
        t[op] = op_line_a;
    for (unsigned op = 0xF000; op <= 0xFFFF; op++)
        t[op] = op_line_f;

#define OPS_CALL(name) ops_##name##_init(&tb);
    OPS_GROUPS(OPS_CALL)
#undef OPS_CALL
    return tb.conflicts;
}
