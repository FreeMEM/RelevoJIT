/* SPDX-License-Identifier: MPL-2.0
 *
 * Exception processing, shared by the engines: groups 1 and 2 (short
 * frame; format $0, or $2 on the 68020+), group 0 (address and bus errors:
 * the 68000's long frame, format $8 on the 68010, $B on the 68020/030, $2
 * or $7 on the 68040), trace and interrupts.
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

/* Exceptions that stack format $2 (with the instruction's address) from
 * the 68020 on: zero divide, CHK and CHK2, TRAPV and TRAPcc, trace. */
static bool format2_vector(unsigned vector)
{
    return vector == VEC_ZERO_DIVIDE || vector == VEC_CHK
        || vector == VEC_TRAPV || vector == VEC_TRACE;
}

/* The 68000 frame (PC, SR), the 68010+ format $0 frame (PC, SR,
 * format/vector word), or format $2 on the 68020+: the same and an
 * address, that of the instruction that caused it on the 68020 and 68030,
 * the next one (the stacked PC) on the 68040 and 68060, as FS-UAE, the
 * bench's oracle, does. */
void cpu_exception(relevo_cpu *c, unsigned vector, uint32_t pc)
{
    bool irq = vector > VEC_SPURIOUS && vector < VEC_TRAP;
    c->cycles += refill_cycles(c) + (irq ? 16 : 6);
    uint16_t old_sr = enter_supervisor(c);
    if (c->model >= RELEVO_68020 && format2_vector(vector)) {
        cpu_push32(c, c->model >= RELEVO_68040 ? pc : c->insn_pc);
        cpu_push16(c, 0x2000 | ((vector * 4) & 0x0FFF));
    } else if (c->model != RELEVO_68000) {
        cpu_push16(c, (vector * 4) & 0x0FFF);   /* format 0 */
    }
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

/* Pushes a frame given as words from its lowest address up. */
static void push_frame(relevo_cpu *c, const uint16_t *w, unsigned n)
{
    while (n > 0)
        cpu_push16(c, w[--n]);
}

static void put32(uint16_t *w, uint32_t v)
{
    w[0] = (uint16_t)(v >> 16);
    w[1] = (uint16_t)v;
}

/* The function code of a faulted access: data or program, user or
 * supervisor (1, 2, 5, 6). */
static unsigned fault_fc(uint16_t sr, unsigned acc)
{
    return (acc & (ACC_DATA | ACC_PROG)) | ((sr & RELEVO_SR_S) ? 4 : 0);
}

/* The size field of the 68020/030 and 68040 special status words: byte 1,
 * word 2, long 0. */
static unsigned size_code(unsigned size)
{
    return size == 4 ? 0 : size & 3;
}

/* Address and bus error frames of the 68010 and later. The frame's PC is
 * the instruction's own address: RTE restarts it (RelevoJIT keeps no
 * internal state to finish a faulted bus cycle). Internal words are 0. */
static void group0_frame(relevo_cpu *c, unsigned vector, uint32_t addr,
                         unsigned acc, uint16_t sr)
{
    uint16_t w[46] = {0};
    unsigned fc = fault_fc(sr, acc), size = c->fault_size;
    bool read = acc & ACC_READ, prog = acc & ACC_PROG;
    uint16_t fv = (uint16_t)((vector * 4) & 0x0FFF);
    uint32_t pc = c->insn_pc;
    w[0] = sr;
    put32(&w[1], pc);

    switch (c->model) {
    case RELEVO_68010:
        /* Format $8 (MC68010 5.5.4): SSW with RR, IF, DF, RM, HB, BY, RW,
         * FC; the fault address; the rest is internal. */
        w[3] = 0x8000 | fv;
        w[4] = (uint16_t)((prog ? 0x2000 : 0x1000) | (size == 1 ? 0x0200 : 0)
                          | (read ? 0x0100 : 0) | fc);
        put32(&w[5], addr);
        push_frame(c, w, 29);
        return;
    case RELEVO_68020:
    case RELEVO_68030:
        /* Format $B, long bus cycle fault (MC68020UM 6.4.4): SSW at +10
         * (FB and RB for a prefetch, DF for data; RW, SIZE, FC), the data
         * cycle fault address at +16, the stage B address at +36. */
        w[3] = 0xB000 | fv;
        w[5] = (uint16_t)((prog ? 0x5000 : 0x0100) | (read ? 0x0040 : 0)
                          | size_code(size) << 4 | fc);
        put32(&w[8], addr);
        put32(&w[18], prog ? addr : c->r.pc);
        push_frame(c, w, 46);
        return;
    default:
        if (vector == VEC_ADDRESS_ERROR) {
            /* Format $2 with the faulted address, bit 0 clear (as
             * FS-UAE, the bench's oracle). */
            w[3] = 0x2000 | fv;
            put32(&w[4], addr & ~1u);
            push_frame(c, w, 6);
            return;
        }
        /* Format $7, access error (MC68040UM 8.4.6): effective address
         * at +8, SSW at +12 (RW, SIZE, TT 0, TM), fault address at +20. */
        w[3] = 0x7000 | fv;
        put32(&w[4], addr);
        w[6] = (uint16_t)((read ? 0x0100 : 0) | size_code(size) << 5 | fc);
        put32(&w[10], addr);
        push_frame(c, w, 30);
        return;
    }
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
    if (c->model == RELEVO_68000) {
        cpu_push32(c, c->ae_pc);
        cpu_push16(c, old_sr);
        cpu_push16(c, c->ir);
        cpu_push32(c, addr);
        cpu_push16(c, ssw);
    } else {
        group0_frame(c, vector, addr, acc, old_sr);
    }
    take_vector(c, vector);
    c->in_group0 = false;
    longjmp(c->fault_jmp, 1);
}

void cpu_address_error(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    c->fault_size = 2;
    group0(c, VEC_ADDRESS_ERROR, addr, acc);
}

void cpu_bus_error(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    group0(c, VEC_BUS_ERROR, addr, acc);
}

/* Autovectored interrupt: the new mask is the level taken. On the 68020
 * to 68040 with M set, the frame goes on the master stack, then M is
 * cleared and a format $1 throwaway frame goes on the interrupt stack
 * (MC68020UM 6.1.7). */
static void interrupt(relevo_cpu *c, unsigned level)
{
    uint32_t pc = c->r.pc;
    unsigned vector = VEC_AUTOVECTOR + level;
    cpu_exception(c, vector, pc);
    if ((c->r.sr & RELEVO_SR_M) && c->model >= RELEVO_68020
        && c->model <= RELEVO_68040) {
        uint16_t sr = c->r.sr;
        cpu_set_sr(c, (uint16_t)(sr & ~RELEVO_SR_M));
        cpu_push16(c, 0x1000 | ((vector * 4) & 0x0FFF));
        cpu_push32(c, pc);
        cpu_push16(c, sr);
    }
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
    if (ipl > mask || nmi) {
        /* A fault while taking it stacks the PC of the instruction it
         * interrupts, as if that one had started (the JIT does not keep
         * these up to date inside blocks). */
        c->insn_pc = c->r.pc;
        c->ae_pc = c->r.pc + 2;
        interrupt(c, ipl);
    }
    return !c->stopped;
}
