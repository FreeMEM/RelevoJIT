/* SPDX-License-Identifier: MPL-2.0
 *
 * Internal state of a CPU, shared by the engines. Not part of the API.
 */
#ifndef RELEVO_CPU_H
#define RELEVO_CPU_H

#include <setjmp.h>
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

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN _Noreturn
#endif

/* Exception vector numbers */
enum {
    VEC_RESET_SP = 0,
    VEC_RESET_PC = 1,
    VEC_BUS_ERROR = 2,
    VEC_ADDRESS_ERROR = 3,
    VEC_ILLEGAL = 4,
    VEC_ZERO_DIVIDE = 5,
    VEC_CHK = 6,
    VEC_TRAPV = 7,
    VEC_PRIVILEGE = 8,
    VEC_TRACE = 9,
    VEC_LINE_A = 10,
    VEC_LINE_F = 11,
    VEC_FORMAT_ERROR = 14,
    VEC_SPURIOUS = 24,
    VEC_AUTOVECTOR = 24,        /* + interrupt level 1-7 */
    VEC_TRAP = 32               /* + 0-15 */
};

/* What a faulting access was doing: OR one of ACC_DATA/ACC_PROG (the low
 * bit of the function code) with ACC_READ or ACC_WRITE. The values are the
 * bits of the 68000's special status word. */
enum {
    ACC_DATA = 1,
    ACC_PROG = 2,
    ACC_WRITE = 0,
    ACC_READ = 0x10
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
    bool halted;                /* double bus fault, until a reset */
    bool align_check;           /* word/long data at odd addresses fault */
    uint64_t count;             /* instructions executed */
    /* Clock cycles, 68000 timing: 4 per bus access of a byte or word (8 for
     * a long), counted by the cpu_read*()/cpu_write*() accesses below, plus
     * what each handler adds for internal operations. Every word of an
     * instruction is counted once when fetched: this stands for the 68000
     * refilling its prefetch queue. */
    uint64_t cycles;
    /* Cycles a group 0 exception adds to or takes from the instruction's
     * count, where the 68000's prefetches are ahead of or behind the
     * words counted so far (MOVE). */
    int fault_cycles;
    uint64_t budget;            /* left in the current relevo_run() */

    /* Trace: set at the start of an instruction from SR.T1, taken before
     * the next one unless the instruction cleared it (group 0 and group 1
     * exceptions do). */
    bool trace_pending;

    /* Group 0 exceptions (address and bus errors) abandon the instruction:
     * they stack their frame and longjmp() back to relevo_run(). */
    bool running;               /* fault_jmp is valid */
    bool in_insn;               /* between fetch and the end of a handler */
    bool in_group0;             /* a fault now is a double bus fault */
    jmp_buf fault_jmp;
    /* What the 68000 stacks in a group 0 frame besides the address: the
     * instruction register (normally the opcode) and the PC at the fault,
     * which depends on how far the instruction got (see interp/ea.c). */
    uint16_t ir;
    uint32_t ae_pc;
    /* Bytes of the access that took a bus error (for the 68010+ frames). */
    unsigned fault_size;

    /* FPU (interp/ops_fpu.c): the vector of an enabled exception to take
     * before the next floating-point instruction (0: none), and whether
     * FSAVE gives a null frame (after a reset, until an FPU instruction). */
    unsigned fpu_pending;
    bool fpu_null;

    atomic_uint ipl;
    unsigned prev_ipl;          /* level 7 is edge triggered */
    atomic_bool stop_request;

    /* JIT engine (NULL with the interpreter). `attention` is set by
     * relevo_set_ipl() and relevo_request_stop(): a block that loops on
     * itself leaves when it sees it. */
    relevo_engine engine;
    struct jit_state *jit;
    atomic_uint attention;

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

/* ---- The CPU's own accesses ----------------------------------------
 *
 * Unlike mem_read*() and mem_write*(), these may raise a group 0
 * exception: an address error for a word or long access at an odd address
 * (68000 and 68010), a bus error for an unmapped page. They must only be
 * used while the CPU runs (inside relevo_run()). `acc` is ACC_DATA or
 * ACC_PROG. Addresses are given unmasked: an address error stacks the
 * whole 32 bits.
 */

/* memory.c */
uint32_t cpu_read_slow(relevo_cpu *cpu, uint32_t addr, unsigned size,
                       unsigned acc);
void cpu_write_slow(relevo_cpu *cpu, uint32_t addr, uint32_t value,
                    unsigned size, unsigned acc);

/* exception.c */
NORETURN void cpu_address_error(relevo_cpu *cpu, uint32_t addr, unsigned acc);
NORETURN void cpu_bus_error(relevo_cpu *cpu, uint32_t addr, unsigned acc);

static inline bool in_page(uint32_t masked, unsigned size)
{
    return (masked & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - size;
}

static inline uint32_t cpu_read8(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    c->cycles += 4;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[a >> RELEVO_PAGE_SHIFT])))
        return c->mem[a];
    return cpu_read_slow(c, addr, 1, acc);
}

static inline uint32_t cpu_read16(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    if (UNLIKELY(addr & 1) && c->align_check)
        cpu_address_error(c, addr, acc | ACC_READ);
    c->cycles += 4;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[a >> RELEVO_PAGE_SHIFT])
               && in_page(a, 2))) {
        const uint8_t *p = c->mem + a;
        return (uint32_t)p[0] << 8 | p[1];
    }
    return cpu_read_slow(c, addr, 2, acc);
}

static inline uint32_t cpu_read32(relevo_cpu *c, uint32_t addr, unsigned acc)
{
    if (UNLIKELY(addr & 1) && c->align_check)
        cpu_address_error(c, addr, acc | ACC_READ);
    c->cycles += 8;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(page_direct_read(c->pages[a >> RELEVO_PAGE_SHIFT])
               && in_page(a, 4))) {
        const uint8_t *p = c->mem + a;
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
             | (uint32_t)p[2] << 8 | p[3];
    }
    return cpu_read_slow(c, addr, 4, acc);
}

static inline void cpu_write8(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    c->cycles += 4;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(c->pages[a >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM))
        c->mem[a] = (uint8_t)v;
    else
        cpu_write_slow(c, addr, v, 1, ACC_DATA);
}

static inline void cpu_write16(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    if (UNLIKELY(addr & 1) && c->align_check)
        cpu_address_error(c, addr, ACC_DATA | ACC_WRITE);
    c->cycles += 4;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(c->pages[a >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM
               && in_page(a, 2))) {
        uint8_t *p = c->mem + a;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        cpu_write_slow(c, addr, v, 2, ACC_DATA);
    }
}

static inline void cpu_write32(relevo_cpu *c, uint32_t addr, uint32_t v)
{
    if (UNLIKELY(addr & 1) && c->align_check)
        cpu_address_error(c, addr, ACC_DATA | ACC_WRITE);
    c->cycles += 8;
    uint32_t a = addr & c->addr_mask;
    if (LIKELY(c->pages[a >> RELEVO_PAGE_SHIFT] == RELEVO_PAGE_RAM
               && in_page(a, 4))) {
        uint8_t *p = c->mem + a;
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    } else {
        cpu_write_slow(c, addr, v, 4, ACC_DATA);
    }
}

/* cpu.c */
void cpu_set_sr(relevo_cpu *cpu, uint16_t sr);

/* exception.c */

/* Pushes on the active stack (a[7]) with the CPU's own accesses. */
void cpu_push16(relevo_cpu *cpu, uint32_t v);
void cpu_push32(relevo_cpu *cpu, uint32_t v);

/* Group 1 and 2 exceptions: stacks the short frame with `pc` as the return
 * address and jumps through the vector. Group 1 exceptions (illegal,
 * privilege, line A/F) also cancel a pending trace: use
 * cpu_exception_group1() for them. */
void cpu_exception(relevo_cpu *cpu, unsigned vector, uint32_t pc);
void cpu_exception_group1(relevo_cpu *cpu, unsigned vector);

/* Between instructions: a pending trace, then interrupts. Returns false if
 * the CPU stays stopped. */
bool cpu_between_insns(relevo_cpu *cpu);

/* Whether cpu_between_insns() has anything to do. */
static inline bool cpu_between_needed(relevo_cpu *c)
{
    unsigned ipl = atomic_load_explicit(&c->ipl, memory_order_relaxed);
    return c->trace_pending || c->stopped || ipl != c->prev_ipl
        || ipl > (c->r.sr & RELEVO_SR_IPL) >> 8u;
}

/* cpu.c: runs one instruction in the interpreter (the checks between
 * instructions are the caller's), counting it against the budget. */
void cpu_interp_step(relevo_cpu *cpu);

/* interp/table.c: fills the table; returns how many opcodes two groups
 * claimed (0 unless there is a bug). */
unsigned interp_fill_table(relevo_handler *table, relevo_model model,
                           relevo_fpu fpu);

/* interp/ops_fpu.c: the FPU after a reset (registers NaN, FPCR, FPSR and
 * FPIAR 0, null state). */
void fpu_reset(relevo_cpu *cpu);

#endif /* RELEVO_CPU_H */
