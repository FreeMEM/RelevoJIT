/* SPDX-License-Identifier: MPL-2.0
 *
 * Program control instructions and the illegal/unimplemented opcodes.
 */
#include "interp.h"

void op_illegal(relevo_cpu *c, uint16_t op)
{
    (void)op;
    cpu_exception(c, VEC_ILLEGAL, c->insn_pc);
}

/* Line A and line F: the host gets the first say (native calls), then the
 * exception, which stacks the address of the opcode itself. */
static void line_trap(relevo_cpu *c, uint16_t op,
                      relevo_trap_result (*fn)(void *, relevo_cpu *, uint16_t),
                      unsigned vector)
{
    if (fn != NULL) {
        switch (fn(c->host.ctx, c, op)) {
        case RELEVO_TRAP_HANDLED:
            return;
        case RELEVO_TRAP_STOP:
            c->exit = RELEVO_HOST_STOP;
            c->exiting = true;
            return;
        default:
            break;
        }
    }
    cpu_exception(c, vector, c->insn_pc);
}

void op_line_a(relevo_cpu *c, uint16_t op)
{
    line_trap(c, op, c->host.line_a, VEC_LINE_A);
}

void op_line_f(relevo_cpu *c, uint16_t op)
{
    line_trap(c, op, c->host.line_f, VEC_LINE_F);
}

void op_nop(relevo_cpu *c, uint16_t op)
{
    (void)c;
    (void)op;
}

void op_rts(relevo_cpu *c, uint16_t op)
{
    (void)op;
    c->r.pc = mem_read32(c, c->r.a[7]);
    c->r.a[7] += 4;
}
