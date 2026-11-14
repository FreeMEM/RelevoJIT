/* SPDX-License-Identifier: MPL-2.0
 *
 * RelevoJIT: a Motorola 680x0 interpreter and x86-64 JIT.
 *
 * This is the whole public interface. The library has no global state: each
 * relevo_cpu is independent, and several can run at the same time, each on
 * its own thread.
 *
 * Memory model
 * ------------
 * The host gives one block of memory (`memory`, `memory_size` bytes). The 68k
 * address `a` lives at `memory + a`, in big-endian byte order, as on the real
 * CPU. The 4 GB address space is split into 65 536 pages of 64 KB. Each page
 * is one of:
 *
 *   RELEVO_PAGE_RAM   read and written directly in `memory`;
 *   RELEVO_PAGE_ROM   read directly, writes are dropped;
 *   RELEVO_PAGE_HOST  every access calls the host's read/write functions
 *                     (chip registers, I/O);
 *   RELEVO_PAGE_NONE  nothing there (a bus error once exceptions are complete;
 *                     for now reads give 0 and writes are dropped).
 *
 * relevo_create() maps every whole page inside `memory_size` as RAM and the
 * rest as NONE. relevo_map() changes it. RAM and ROM pages must lie inside
 * `memory_size`.
 *
 * Addresses are masked with the CPU's address mask before use: 24 bits on the
 * 68000 and 68010, 32 bits on the others (see relevo_config.address_mask).
 *
 * Registers
 * ---------
 * relevo_get_regs() returns a pointer to the live registers. The host may read
 * and change them while the CPU is not running, and from inside its callbacks.
 * a[7] is always the active stack pointer; the copy of the active one among
 * usp/isp/msp is stale until the CPU switches stacks. To change the S or M
 * bits use relevo_set_sr(), which switches stacks as the CPU does; writing
 * regs->sr directly does not.
 */
#ifndef RELEVO_H
#define RELEVO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELEVO_VERSION_MAJOR 0
#define RELEVO_VERSION_MINOR 1

typedef struct relevo_cpu relevo_cpu;

/* ---- Configuration ---------------------------------------------------- */

typedef enum relevo_model {
    RELEVO_68000 = 0,
    RELEVO_68010,
    RELEVO_68020,
    RELEVO_68030,
    RELEVO_68040,
    RELEVO_68060
} relevo_model;

typedef enum relevo_fpu {
    RELEVO_FPU_NONE = 0,
    RELEVO_FPU_68881,       /* 68020 or 68030 with a 68881 */
    RELEVO_FPU_68882,       /* 68020 or 68030 with a 68882 */
    RELEVO_FPU_INTERNAL     /* the built-in FPU of the 68040 or 68060 */
} relevo_fpu;

typedef enum relevo_engine {
    RELEVO_ENGINE_INTERP = 0,   /* portable interpreter */
    RELEVO_ENGINE_JIT           /* x86-64 JIT (not available yet) */
} relevo_engine;

/* What the host's line A / line F function asks the CPU to do. */
typedef enum relevo_trap_result {
    /* Not handled: take the line A (vector 10) or line F (vector 11)
     * exception, as a real 68k does. */
    RELEVO_TRAP_EXCEPTION = 0,
    /* Handled: go on at regs->pc. */
    RELEVO_TRAP_HANDLED,
    /* Handled, and relevo_run() must return RELEVO_HOST_STOP now. */
    RELEVO_TRAP_STOP
} relevo_trap_result;

/* Functions the host provides. All are optional (NULL). They run on the
 * thread that called relevo_run() and must not call relevo_run() again. */
typedef struct relevo_host {
    void *ctx;  /* passed back to every function */

    /* Accesses to RELEVO_PAGE_HOST pages. `size` is 1, 2 or 4 bytes; the
     * value is in the CPU's order (the most significant byte is the lowest
     * address). An access that crosses a page boundary is split into byte
     * accesses. Bits of a read result above `size` bytes are ignored.
     * Without `read`, reads give 0; without `write`, writes are dropped. */
    uint32_t (*read)(void *ctx, uint32_t address, unsigned size);
    void (*write)(void *ctx, uint32_t address, uint32_t value, unsigned size);

    /* Called for an opcode 0xAxxx (line A) or for a 0xFxxx (line F) that the
     * configured CPU and FPU do not execute, before any exception. On entry
     * regs->pc is the address after the opcode word; the function may change
     * any register. */
    relevo_trap_result (*line_a)(void *ctx, relevo_cpu *cpu, uint16_t opcode);
    relevo_trap_result (*line_f)(void *ctx, relevo_cpu *cpu, uint16_t opcode);
} relevo_host;

/* relevo_config.options */

/* relevo_run() returns RELEVO_EXIT_PC_ZERO as soon as an instruction leaves
 * the PC at address 0 (an RTS to 0, a JMP 0...). The host can push 0 as the
 * return address of the code it calls, and gets control back when that code
 * returns. This is what Emu68 does. Off by default. */
#define RELEVO_OPT_EXIT_ON_PC_ZERO (1u << 0)

/* A zeroed relevo_config is valid: a 68000 without FPU, interpreter, no
 * memory, no host functions. */
typedef struct relevo_config {
    relevo_model model;
    relevo_fpu fpu;
    relevo_engine engine;

    uint8_t *memory;        /* big-endian 68k memory, address 0 at memory[0] */
    size_t memory_size;     /* a multiple of RELEVO_PAGE_SIZE, at most 4 GB */

    /* Mask applied to every address. 0 means the model's default:
     * 0x00FFFFFF for the 68000 and 68010, 0xFFFFFFFF for the others. */
    uint32_t address_mask;

    unsigned options;       /* RELEVO_OPT_* */

    relevo_host host;
} relevo_config;

/* ---- Memory pages ----------------------------------------------------- */

#define RELEVO_PAGE_SHIFT 16
#define RELEVO_PAGE_SIZE  (1u << RELEVO_PAGE_SHIFT)     /* 64 KB */
#define RELEVO_PAGE_COUNT (1u << (32 - RELEVO_PAGE_SHIFT))

typedef enum relevo_page_kind {
    RELEVO_PAGE_NONE = 0,
    RELEVO_PAGE_RAM,
    RELEVO_PAGE_ROM,
    RELEVO_PAGE_HOST
} relevo_page_kind;

/* ---- Registers -------------------------------------------------------- */

/* SR bits */
#define RELEVO_SR_C   0x0001u
#define RELEVO_SR_V   0x0002u
#define RELEVO_SR_Z   0x0004u
#define RELEVO_SR_N   0x0008u
#define RELEVO_SR_X   0x0010u
#define RELEVO_SR_IPL 0x0700u   /* interrupt mask, bits 8-10 */
#define RELEVO_SR_M   0x1000u   /* master/interrupt state (68020-68040) */
#define RELEVO_SR_S   0x2000u   /* supervisor */
#define RELEVO_SR_T0  0x4000u   /* trace on change of flow (68020-68040) */
#define RELEVO_SR_T1  0x8000u   /* trace */

/* An FPU register in the 68881 extended format: sign and 15-bit exponent,
 * then a 64-bit mantissa with an explicit integer bit. */
typedef struct relevo_fpreg {
    uint64_t mantissa;
    uint16_t sign_exponent;
} relevo_fpreg;

typedef struct relevo_regs {
    uint32_t d[8];
    uint32_t a[8];          /* a[7] is the active stack pointer */
    uint32_t pc;
    uint16_t sr;
    uint32_t usp;           /* user stack pointer */
    uint32_t isp;           /* interrupt stack pointer (the SSP on 68000/010) */
    uint32_t msp;           /* master stack pointer (68020-68040) */
    uint32_t vbr;           /* 68010 and later */
    uint32_t cacr, caar;    /* 68020 and later */
    uint32_t sfc, dfc;      /* 68010 and later */
    relevo_fpreg fp[8];
    uint32_t fpcr, fpsr, fpiar;
} relevo_regs;

/* ---- Running ---------------------------------------------------------- */

/* Why relevo_run() returned. */
typedef enum relevo_status {
    RELEVO_BUDGET = 0,      /* the budget is spent */
    RELEVO_EXIT_PC_ZERO,    /* PC reached 0 (RELEVO_OPT_EXIT_ON_PC_ZERO) */
    RELEVO_STOPPED,         /* STOP: waiting for an interrupt */
    RELEVO_HOST_STOP,       /* relevo_request_stop() or RELEVO_TRAP_STOP */
    RELEVO_HALTED           /* double bus fault: only a reset gets out */
} relevo_status;

/* Creates a CPU. The configuration is copied. Returns NULL if the
 * configuration is not valid or not supported yet (the JIT engine, for now). */
relevo_cpu *relevo_create(const relevo_config *config);
void relevo_destroy(relevo_cpu *cpu);

/* Maps [address, address + size) as `kind`. Both must be multiples of
 * RELEVO_PAGE_SIZE (size may be 1 << 32 to cover everything). Returns 0, or
 * -1 if not aligned or if RAM/ROM would fall outside the memory block. */
int relevo_map(relevo_cpu *cpu, uint32_t address, uint64_t size,
               relevo_page_kind kind);

relevo_regs *relevo_get_regs(relevo_cpu *cpu);

/* Sets SR as a MOVE to SR does: masks the bits the model lacks and switches
 * a[7] between usp, isp and msp. */
void relevo_set_sr(relevo_cpu *cpu, uint16_t sr);

/* Reset: supervisor mode, interrupts masked (SR = 0x2700), VBR = 0, ISP and
 * PC read from addresses 0 and 4. */
void relevo_reset(relevo_cpu *cpu);

/* Runs until `budget` instructions have executed or one of the other
 * reasons in relevo_status happens. */
relevo_status relevo_run(relevo_cpu *cpu, uint64_t budget);

/* Instructions executed since the CPU was created. */
uint64_t relevo_instructions(const relevo_cpu *cpu);

/* Sets the interrupt level the CPU sees on its IPL pins (0 to 7). The CPU
 * takes the interrupt between instructions. Safe from any thread. */
void relevo_set_ipl(relevo_cpu *cpu, unsigned level);

/* Makes relevo_run() return RELEVO_HOST_STOP before the next instruction.
 * Safe from any thread and from host functions. */
void relevo_request_stop(relevo_cpu *cpu);

/* Tells the CPU that 68k code in [address, address + size) changed, so that
 * translated code is dropped. The interpreter needs nothing. */
void relevo_invalidate(relevo_cpu *cpu, uint32_t address, uint32_t size);

/* Memory accesses as the CPU does them (page table, address mask, host
 * functions), for use by host functions. `size` is 1, 2 or 4. */
uint32_t relevo_read(relevo_cpu *cpu, uint32_t address, unsigned size);
void relevo_write(relevo_cpu *cpu, uint32_t address, uint32_t value,
                  unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* RELEVO_H */
