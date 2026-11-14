/* SPDX-License-Identifier: MPL-2.0
 *
 * CPU creation, the public API and exception processing.
 */
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

static bool config_valid(const relevo_config *cfg)
{
    if (cfg->model > RELEVO_68060 || cfg->fpu > RELEVO_FPU_INTERNAL)
        return false;
    if (cfg->engine != RELEVO_ENGINE_INTERP)
        return false;   /* the JIT does not exist yet */
    if (cfg->memory_size % RELEVO_PAGE_SIZE != 0
        || (uint64_t)cfg->memory_size > ((uint64_t)1 << 32))
        return false;
    if (cfg->memory_size != 0 && cfg->memory == NULL)
        return false;
    switch (cfg->fpu) {
    case RELEVO_FPU_68881:
    case RELEVO_FPU_68882:
        return cfg->model == RELEVO_68020 || cfg->model == RELEVO_68030;
    case RELEVO_FPU_INTERNAL:
        return cfg->model == RELEVO_68040 || cfg->model == RELEVO_68060;
    default:
        return true;
    }
}

relevo_cpu *relevo_create(const relevo_config *cfg)
{
    if (cfg == NULL || !config_valid(cfg))
        return NULL;

    relevo_cpu *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;
    c->table = malloc(sizeof(relevo_handler) * 0x10000);
    if (c->table == NULL) {
        free(c);
        return NULL;
    }

    c->mem = cfg->memory;
    c->mem_size = cfg->memory_size;
    c->model = cfg->model;
    c->fpu = cfg->fpu;
    c->options = cfg->options;
    c->host = cfg->host;
    c->addr_mask = cfg->address_mask;
    if (c->addr_mask == 0)
        c->addr_mask = c->model <= RELEVO_68010 ? 0x00FFFFFFu : 0xFFFFFFFFu;

    switch (c->model) {
    case RELEVO_68020:
    case RELEVO_68030:
    case RELEVO_68040:
        c->sr_mask = 0xF71F;    /* T1 T0 S M */
        break;
    default:
        c->sr_mask = 0xA71F;    /* T1 S */
        break;
    }

    atomic_init(&c->ipl, 0);
    atomic_init(&c->stop_request, false);

    if (cfg->memory_size != 0)
        relevo_map(c, 0, cfg->memory_size, RELEVO_PAGE_RAM);

    interp_fill_table(c->table, c->model, c->fpu);

    c->r.sr = RELEVO_SR_S | RELEVO_SR_IPL;
    return c;
}

void relevo_destroy(relevo_cpu *c)
{
    if (c == NULL)
        return;
    free(c->table);
    free(c);
}

int relevo_map(relevo_cpu *c, uint32_t address, uint64_t size,
               relevo_page_kind kind)
{
    uint64_t end = (uint64_t)address + size;
    if (address % RELEVO_PAGE_SIZE != 0 || size % RELEVO_PAGE_SIZE != 0
        || end > ((uint64_t)1 << 32) || kind > RELEVO_PAGE_HOST)
        return -1;
    /* Direct pages must lie inside the memory block. */
    if ((kind == RELEVO_PAGE_RAM || kind == RELEVO_PAGE_ROM)
        && (c->mem == NULL || end > c->mem_size))
        return -1;
    for (uint64_t p = address >> RELEVO_PAGE_SHIFT;
         p < end >> RELEVO_PAGE_SHIFT; p++)
        c->pages[p] = (uint8_t)kind;
    return 0;
}

relevo_regs *relevo_get_regs(relevo_cpu *c)
{
    return &c->r;
}

/* Where the stack pointer for a given SR is kept while it is not active. */
static uint32_t *sp_slot(relevo_cpu *c, uint16_t sr)
{
    if (!(sr & RELEVO_SR_S))
        return &c->r.usp;
    if (sr & RELEVO_SR_M)
        return &c->r.msp;
    return &c->r.isp;
}

void cpu_set_sr(relevo_cpu *c, uint16_t sr)
{
    sr &= c->sr_mask;
    *sp_slot(c, c->r.sr) = c->r.a[7];
    c->r.sr = sr;
    c->r.a[7] = *sp_slot(c, sr);
}

void relevo_set_sr(relevo_cpu *c, uint16_t sr)
{
    cpu_set_sr(c, sr);
}

void relevo_reset(relevo_cpu *c)
{
    c->r.sr = RELEVO_SR_S | RELEVO_SR_IPL;
    c->r.vbr = 0;
    c->r.cacr = 0;
    c->stopped = false;
    c->r.isp = mem_read32(c, 0);
    c->r.a[7] = c->r.isp;
    c->r.pc = mem_read32(c, 4);
}

void cpu_push16(relevo_cpu *c, uint32_t v)
{
    c->r.a[7] -= 2;
    mem_write16(c, c->r.a[7], v);
}

void cpu_push32(relevo_cpu *c, uint32_t v)
{
    c->r.a[7] -= 4;
    mem_write32(c, c->r.a[7], v);
}

/* Group 1 and 2 exceptions with a short frame: the 68000 frame (PC, SR) or
 * the 68010+ format 0 frame (PC, SR, format/vector word). `pc` is the value
 * stacked as the return address.
 * TODO(R2): group 0 frames, interrupts (M bit), trace, double faults. */
void cpu_exception(relevo_cpu *c, unsigned vector, uint32_t pc)
{
    uint16_t old_sr = c->r.sr;
    cpu_set_sr(c, (uint16_t)((old_sr | RELEVO_SR_S)
                             & ~(RELEVO_SR_T1 | RELEVO_SR_T0)));
    if (c->model != RELEVO_68000)
        cpu_push16(c, (vector * 4) & 0x0FFF);  /* format 0 */
    cpu_push32(c, pc);
    cpu_push16(c, old_sr);
    uint32_t vbr = c->model == RELEVO_68000 ? 0 : c->r.vbr;
    c->r.pc = mem_read32(c, vbr + vector * 4);
}

relevo_status relevo_run(relevo_cpu *c, uint64_t budget)
{
    if (c->stopped)
        return RELEVO_STOPPED;  /* TODO(R2): wake up on an interrupt */
    c->exiting = false;
    for (; budget != 0; budget--) {
        if (UNLIKELY(atomic_load_explicit(&c->stop_request,
                                          memory_order_relaxed))) {
            atomic_store_explicit(&c->stop_request, false,
                                  memory_order_relaxed);
            return RELEVO_HOST_STOP;
        }
        /* TODO(R2): interrupts, trace */
        c->insn_pc = c->r.pc;
        uint16_t op = (uint16_t)mem_read16(c, c->r.pc);
        c->r.pc += 2;
        c->table[op](c, op);
        c->count++;
        if (UNLIKELY(c->exiting))
            return c->exit;
        if (UNLIKELY(c->r.pc == 0 && (c->options & RELEVO_OPT_EXIT_ON_PC_ZERO)))
            return RELEVO_EXIT_PC_ZERO;
    }
    return RELEVO_BUDGET;
}

uint64_t relevo_instructions(const relevo_cpu *c)
{
    return c->count;
}

void relevo_set_ipl(relevo_cpu *c, unsigned level)
{
    atomic_store_explicit(&c->ipl, level & 7, memory_order_relaxed);
}

void relevo_request_stop(relevo_cpu *c)
{
    atomic_store_explicit(&c->stop_request, true, memory_order_relaxed);
}

void relevo_invalidate(relevo_cpu *c, uint32_t address, uint32_t size)
{
    /* The interpreter keeps no translated code. */
    (void)c;
    (void)address;
    (void)size;
}

uint32_t relevo_read(relevo_cpu *c, uint32_t address, unsigned size)
{
    switch (size) {
    case 1: return mem_read8(c, address);
    case 2: return mem_read16(c, address);
    case 4: return mem_read32(c, address);
    default: return 0;
    }
}

void relevo_write(relevo_cpu *c, uint32_t address, uint32_t value,
                  unsigned size)
{
    switch (size) {
    case 1: mem_write8(c, address, value); break;
    case 2: mem_write16(c, address, value); break;
    case 4: mem_write32(c, address, value); break;
    default: break;
    }
}
