/* SPDX-License-Identifier: MPL-2.0
 *
 * Internal state of a CPU, shared by the engines. Not part of the API.
 */
#ifndef RELEVO_CPU_H
#define RELEVO_CPU_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "relevo.h"

#if defined(__GNUC__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

/* Exception vector numbers */
enum {
    VEC_RESET_SP = 0,
    VEC_RESET_PC = 1,
    VEC_BUS_ERROR = 2,
    VEC_ADDRESS_ERROR = 3,
    VEC_ILLEGAL = 4,
    VEC_LINE_A = 10,
    VEC_LINE_F = 11
};

/* One handler per opcode. The opcode word is already fetched and regs.pc
 * points after it. */
typedef void (*relevo_handler)(relevo_cpu *cpu, uint16_t opcode);

struct relevo_cpu {
    relevo_regs r;              /* first: the JIT will address it from a base */

    uint8_t *mem;
    uint64_t mem_size;
    uint32_t addr_mask;
    uint16_t sr_mask;           /* SR bits the model implements */
    relevo_model model;
    relevo_fpu fpu;
    unsigned options;
    relevo_host host;

    uint32_t insn_pc;           /* address of the instruction being run */
    relevo_status exit;         /* set by a handler to end relevo_run() */
    bool exiting;
    bool stopped;               /* after STOP, until an interrupt */
    uint64_t count;             /* instructions executed */

    atomic_uint ipl;
    atomic_bool stop_request;

    uint8_t pages[RELEVO_PAGE_COUNT];   /* relevo_page_kind per page */
    relevo_handler *table;              /* 65 536 handlers */
};

/* memory.c: slow paths, for anything but a direct access inside one page */
uint32_t mem_read_slow(relevo_cpu *cpu, uint32_t addr, unsigned size);
void mem_write_slow(relevo_cpu *cpu, uint32_t addr, uint32_t value,
                    unsigned size);

static inline bool page_direct_read(uint8_t kind)
{
    return kind == RELEVO_PAGE_RAM || kind == RELEVO_PAGE_ROM;
}

static inline uint32_t mem_read8(relevo_cpu *c, uint32_t addr)
{
    addr &= c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[addr >> RELEVO_PAGE_SHIFT])))
        return c->mem[addr];
    return mem_read_slow(c, addr, 1);
}

static inline uint32_t mem_read16(relevo_cpu *c, uint32_t addr)
{
    addr &= c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[addr >> RELEVO_PAGE_SHIFT])
               && (addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - 2)) {
        const uint8_t *p = c->mem + addr;
        return (uint32_t)p[0] << 8 | p[1];
    }
    return mem_read_slow(c, addr, 2);
}

static inline uint32_t mem_read32(relevo_cpu *c, uint32_t addr)
{
    addr &= c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[addr >> RELEVO_PAGE_SHIFT])
               && (addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - 4)) {
        const uint8_t *p = c->mem + addr;
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
             | (uint32_t)p[2] << 8 | p[3];
    }
    return mem_read_slow(c, addr, 4);
}

static inline void mem_write8(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    addr &= c->addr_mask;
    if (LIKELY(c->pages[addr >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM))
        c->mem[addr] = (uint8_t)v;
    else
        mem_write_slow(c, addr, v, 1);
}

static inline void mem_write16(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    addr &= c->addr_mask;
    if (LIKELY(c->pages[addr >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM
               && (addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - 2)) {
        uint8_t *p = c->mem + addr;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        mem_write_slow(c, addr, v, 2);
    }
}

static inline void mem_write32(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    addr &= c->addr_mask;
    if (LIKELY(c->pages[addr >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM
               && (addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - 4)) {
        uint8_t *p = c->mem + addr;
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    } else {
        mem_write_slow(c, addr, v, 4);
    }
}

/* cpu.c */
void cpu_set_sr(relevo_cpu *cpu, uint16_t sr);
void cpu_push16(relevo_cpu *cpu, uint32_t v);
void cpu_push32(relevo_cpu *cpu, uint32_t v);
void cpu_exception(relevo_cpu *cpu, unsigned vector, uint32_t pc);

/* interp/table.c */
void interp_fill_table(relevo_handler *table, relevo_model model,
                       relevo_fpu fpu);

#endif /* RELEVO_CPU_H */
