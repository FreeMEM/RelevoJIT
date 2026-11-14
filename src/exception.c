/* SPDX-License-Identifier: MPL-2.0
 *
 * Exception processing, shared by the engines: groups 1 and 2 (short
 * frame), group 0 (address and bus errors, long frame on the 68000), trace
 * and interrupts.
 *
 * Group 0 exceptions abandon the instruction half done. They are raised
 * from inside memory accesses, stack their frame and longjmp() back to
 * relevo_run(), which goes on with the next instruction (or returns
 * RELEVO_HALTED after a double bus fault).
 */
#include "cpu.h"

void cpu_push16(relevo_cpu *c, uint32_t v)
{
    c->r.a[7] -= 2;
    cpu_write16(c, c->r.a[7], v);
}

void cpu_push32(relevo_cpu *c, uint32_t v)
{
    c->r.a[7] -= 4;
    cpu_write32(c, c->r.a[7], v);
}

static uint32_t vector_base(const relevo_cpu *c)
{
    return c->model == RELEVO_68000 ? 0 : c->r.vbr;
}

/* Supervisor mode, trace off; returns the SR to stack. */
static uint16_t enter_supervisor(relevo_cpu *c)
{
    uint16_t old_sr = c->r.sr;
    cpu_set_sr(c, (uint16_t)((old_sr | RELEVO_SR_S)
                             & ~(RELEVO_SR_T1 | RELEVO_SR_T0)));
    return old_sr;
}

static void take_vector(relevo_cpu *c, unsigned vector)
{
    uint32_t pc = cpu_read32(c, vector_base(c) + vector * 4, ACC_DATA);
    c->stopped = false;
    if (pc & 1) {
        /* The first prefetch of the handler fails. */
        c->ae_pc = pc;
        cpu_address_error(c, pc, ACC_PROG | ACC_READ);
    }
    c->r.pc = pc;
}

/* Cycles: an exception refills the prefetch queue from the handler (two
 * words). Between instructions both count; inside one, the instruction's
 * first word, counted when fetched, stands for one of them. */
static unsigned refill_cycles(const relevo_cpu *c)
{
    return c->in_insn ? 4 : 8;
}

/* The 68000 frame (PC, SR) or the 68010+ format 0 frame (PC, SR,
 * format/vector word). */
void cpu_exception(relevo_cpu *c, unsigned vector, uint32_t pc)
{
    bool irq = vector > VEC_SPURIOUS && vector < VEC_TRAP;
    c->cycles += refill_cycles(c) + (irq ? 16 : 6);
    uint16_t old_sr = enter_supervisor(c);
    if (c->model != RELEVO_68000)
        cpu_push16(c, (vector * 4) & 0x0FFF);   /* format 0 */
    cpu_push32(c, pc);
    cpu_push16(c, old_sr);
    take_vector(c, vector);
}

/* Illegal, privilege violation, line A and line F: the instruction does not
 * run, its own address is stacked, and it is not traced. */
void cpu_exception_group1(relevo_cpu *c, unsigned vector)
{
    c->trace_pending = false;
    cpu_exception(c, vector, c->insn_pc);
}

static NORETURN void group0(relevo_cpu *c, unsigned vector, uint32_t addr,
                            unsigned acc)
{
    if (c->in_group0) {
        /* A fault while stacking a fault: the 68000 halts. */
        c->in_group0 = false;
        c->halted = true;
        longjmp(c->fault_jmp, 1);
    }
    c->in_group0 = true;
    c->trace_pending = false;
    /* Internal cycles and the two prefetches from the handler. The words
     * of the instruction counted so far stand for the faulted access. */
    c->cycles += (uint64_t)(18 + c->fault_cycles);

    /* Special status word: the upper bits of IR (undocumented, as the
     * MAME microcode does), R/W, I/N (0) and the function code. */
    uint16_t old_sr = c->r.sr;
    uint16_t ssw = (uint16_t)((c->ir & 0xFFE0) | (acc & ACC_READ)
                              | ((old_sr & RELEVO_SR_S) ? 4 : 0)
                              | (acc & (ACC_DATA | ACC_PROG)));
    enter_supervisor(c);
    /* TODO(R3): the 68010 and later stack formats 8, A and B instead. */
    cpu_push32(c, c->ae_pc);
    cpu_push16(c, old_sr);
    cpu_push16(c, c->ir);
    cpu_push32(c, addr);
    cpu_push16(c, ssw);
    take_vector(c, vector);
    c->in_group0 = false;
    longjmp(c->fault_jmp, 1);
}

void cpu_address_error(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    group0(c, VEC_ADDRESS_ERROR, addr, acc);
}

void cpu_bus_error(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    group0(c, VEC_BUS_ERROR, addr, acc);
}

/* Autovectored interrupt: the new mask is the level taken. */
static void interrupt(relevo_cpu *c, unsigned level)
{
    cpu_exception(c, VEC_AUTOVECTOR + level, c->r.pc);
    c->r.sr = (uint16_t)((c->r.sr & ~RELEVO_SR_IPL) | level << 8);
}

bool cpu_between_insns(relevo_cpu *c)
{
    if (c->trace_pending) {
        c->trace_pending = false;
        cpu_exception(c, VEC_TRACE, c->r.pc);
    }
    unsigned ipl = atomic_load_explicit(&c->ipl, memory_order_relaxed);
    unsigned mask = (c->r.sr & RELEVO_SR_IPL) >> 8;
    bool nmi = ipl == 7 && c->prev_ipl != 7;    /* level 7: on the edge */
    c->prev_ipl = ipl;
    if (ipl > mask || nmi)
        interrupt(c, ipl);
    return !c->stopped;
}
