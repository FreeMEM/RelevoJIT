/* SPDX-License-Identifier: MPL-2.0
 *
 * The random 68k code and state generator shared by the differential tests
 * (jitdiff.c: the JIT against the interpreter; crdiff.c: the Rust library
 * against the C one).
 *
 * A tool sets `model` and `gen_fpu`, calls setup_static() once (vectors,
 * handler and ROM in init_mem) and, for each case, rnd_insn(pc) to get one
 * random instruction in words[0..nwords). The random sequence depends only
 * on rng_state and the calls made.
 */
#ifndef RELEVO_GEN68K_H
#define RELEVO_GEN68K_H

#include <stdbool.h>
#include <stdint.h>

#include "relevo.h"

/* The memory both tools lay out */
#define MEM_SIZE    0x1000000u
#define CODE        0x10000u        /* where the instructions go */
#define CODE_END    0x11000u
#define WIN         0x40000u        /* data the registers point into */
#define WIN_SIZE    0x2000u
#define HANDLER     0x800u          /* every exception vector */
#define ROM_PAGE    0x200000u
#define HOST_PAGE   0x210000u
#define NONE_PAGE   0x220000u

/* The CPU the code is for: what instructions and modes to generate. */
extern relevo_model model;
extern relevo_fpu gen_fpu;

/* ---- Random numbers ---- */

extern uint64_t rng_state;
uint32_t rnd(void);
uint32_t rn(uint32_t n);            /* 0 to n - 1 */
bool chance(unsigned percent);

/* ---- Instructions ---- */

/* Effective address classes (bits as in interp.h) */
enum {
    C_DREG = 1 << 0, C_AREG = 1 << 1, C_IND = 1 << 2, C_POST = 1 << 3,
    C_PRE = 1 << 4, C_D16 = 1 << 5, C_D8 = 1 << 6, C_ABSW = 1 << 7,
    C_ABSL = 1 << 8, C_PCD16 = 1 << 9, C_PCD8 = 1 << 10, C_IMM = 1 << 11,
    C_ALL = 0xFFF,
    C_DATA = C_ALL & ~C_AREG,
    C_MEMALT = C_IND | C_POST | C_PRE | C_D16 | C_D8 | C_ABSW | C_ABSL,
    C_DATAALT = C_MEMALT | C_DREG,
    C_ALT = C_DATAALT | C_AREG,
    C_CONTROL = C_IND | C_D16 | C_D8 | C_ABSW | C_ABSL | C_PCD16 | C_PCD8
};

extern uint16_t words[64];
extern unsigned nwords;
void w16(uint32_t v);
void w32(uint32_t v);

uint32_t rnd_addr(void);
uint32_t rnd_value(void);
/* An effective address of `classes`; returns mode << 3 | reg and appends
 * its extension words. */
unsigned rnd_ea(unsigned classes, unsigned size);
unsigned rnd_size(void);
unsigned size_bits(unsigned size);
int32_t rnd_disp(uint32_t pc);
uint32_t rnd_mask(void);
unsigned rnd_fop(void);
/* Whether an FPU opmode is a transcendental function (68881/68882), and
 * whether the generator leaves them out (crdiff -J: the two libraries
 * compute them differently, and on the JIT a run is compared only at its
 * end). */
bool fop_transcendental(unsigned opmode);
extern bool gen_no_trans;
void rnd_fpu(uint32_t pc, unsigned dn);
void rnd_insn_more(uint32_t pc, unsigned size, unsigned dn);
/* One random instruction for code at `pc`, in words[0..nwords). */
void rnd_insn(uint32_t pc);

/* ---- Memory and FPU values ---- */

extern uint8_t init_mem[MEM_SIZE];
void put16(uint8_t *m, uint32_t a, uint32_t v);
void put32(uint8_t *m, uint32_t a, uint32_t v);
/* Vectors to a handler of NOPs ending in a loop; the ROM page's bytes. */
void setup_static(void);
/* A random extended value: zeros, infinities, NaNs, denormals, small
 * integers and any normal number. */
relevo_fpreg rnd_fp(void);

#endif
