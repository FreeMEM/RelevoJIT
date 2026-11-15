/* SPDX-License-Identifier: MPL-2.0
 *
 * Differential test of the two libraries: the Rust one (relevo_*) against
 * the C one in c/ (linked with its symbols renamed to c_relevo_*; see the
 * Makefile), both on the interpreter (docs/REESCRITURA.md, RR2 and RR4).
 *
 *   crdiff [-m MODEL] [-f FPU] [-a] [-n CASES] [-k MAXLEN] [-s SEED]
 *          [-g PERCENT] [-u ULPS] [-T]
 *
 *   -m  68000 68010 68020 68030 68040 68060 (default 68000)
 *   -f  none 68881 68882 internal (default: none on the 68000 and 68010,
 *       a 68882 on the 68020 and 68030, the internal one on the 68040/060)
 *   -a  every model, with and without its FPUs, CASES each
 *   -g  percentage of instructions that are raw random words (default 10;
 *       100: nothing but garbage as code)
 *   -u  how many units in the last place a transcendental result may differ
 *       (default 2; see below)
 *   -T  no tolerance at all
 *
 * Each case writes 1 to MAXLEN random instructions (the generator of
 * gen68k.c, as jitdiff uses it, plus what the 68010 to 68060 and the FPU
 * add: MOVEC, MOVES, RTD, RTE of random frames, CAS2, CHK2, TRAPcc, MOVE16,
 * CINV/CPUSH, FMOVEM, FMOVECR, FSAVE/FRESTORE, FTRAPcc, FDBcc, packed
 * decimal, the transcendental functions; and garbage), random registers
 * (SR with T1/T0/M/S, VBR, CACR, CAAR, SFC, DFC, the MMU registers, FPU
 * registers, FPCR and FPSR) and data, and runs both CPUs one instruction
 * at a time in lockstep, with interrupt levels that change between
 * instructions. After every instruction it compares all of relevo_regs,
 * the cycles, the instruction count, the run status and the log of host
 * calls (page reads and writes, line A and line F, RESET); after the case,
 * memory (the windows a case reaches; all of it every 256 cases). The
 * first difference is printed with the case and the instruction, and
 * crdiff exits with 1.
 *
 * Transcendental functions (FSIN... FLOG2, FSINCOS: opmodes 0x02, 0x06,
 * 0x08-0x0A, 0x0C-0x0F, 0x10-0x12, 0x14-0x16, 0x19, 0x1C, 0x1D, 0x30-0x37 on
 * a 68881/68882): the C version takes them from the host's long double
 * libm, the Rust one computes them itself, so the last bits may differ.
 * After an instruction that is one of them, the FPU data registers may
 * differ by up to ULPS units in the last place (everything else, the FPSR
 * included, must be equal); then the Rust CPU gets the C one's registers so
 * that the rest of the case compares exactly. The statistics are printed
 * at the end.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relevo.h"
#include "gen68k.h"

/* ---- The two libraries ------------------------------------------------ */

/* The C library, its global symbols renamed with a c_ prefix. */
#define C_DECLARE(ret, name, args) ret c_##name args;
#define API_FUNCS(X)                                                        \
    X(relevo_cpu *, relevo_create, (const relevo_config *))                 \
    X(void, relevo_destroy, (relevo_cpu *))                                 \
    X(int, relevo_map, (relevo_cpu *, uint32_t, uint64_t, relevo_page_kind)) \
    X(relevo_regs *, relevo_get_regs, (relevo_cpu *))                       \
    X(void, relevo_reset, (relevo_cpu *))                                   \
    X(relevo_status, relevo_run, (relevo_cpu *, uint64_t))                  \
    X(uint64_t, relevo_instructions, (const relevo_cpu *))                  \
    X(uint64_t, relevo_cycles, (const relevo_cpu *))                        \
    X(void, relevo_set_ipl, (relevo_cpu *, unsigned))                       \
    X(void, relevo_invalidate, (relevo_cpu *, uint32_t, uint32_t))          \
    X(uint32_t, relevo_read, (relevo_cpu *, uint32_t, unsigned))            \
    X(void, relevo_write, (relevo_cpu *, uint32_t, uint32_t, unsigned))
API_FUNCS(C_DECLARE)

typedef struct api {
    const char *name;
#define API_FIELD(ret, name, args) ret (*name) args;
    API_FUNCS(API_FIELD)
} api;

#define API_RUST(ret, name, args) .name = name,
#define API_C(ret, name, args) .name = c_##name,
static const api rust_api = { "Rust", API_FUNCS(API_RUST) };
static const api c_api = { "C", API_FUNCS(API_C) };

/* ---- The two machines ------------------------------------------------- */

#define LOG_MAX 64

enum { LOG_READ, LOG_WRITE, LOG_LINE_A, LOG_LINE_F, LOG_RESET };

typedef struct access {
    uint32_t addr, value;
    unsigned size, kind;
} access;

typedef struct machine {
    const api *api;
    relevo_cpu *cpu;
    uint8_t *mem;
    access log[LOG_MAX];
    unsigned nlog;
} machine;

static void log_access(machine *m, unsigned kind, uint32_t addr,
                       uint32_t value, unsigned size)
{
    if (m->nlog < LOG_MAX)
        m->log[m->nlog++] = (access){ addr, value, size, kind };
}

/* Some host accesses patch code or remap a page, as a host may. */
static void host_effects(machine *m, uint32_t addr, uint32_t v)
{
    switch (addr & 0x3F) {
    case 0x10: {
        uint32_t a = CODE + (v & 0xFFE);
        m->mem[a] = 0x4E;
        m->mem[a + 1] = 0x71;
        m->api->relevo_invalidate(m->cpu, a, 2);
        break;
    }
    case 0x20:
        m->api->relevo_map(m->cpu, HOST_PAGE, RELEVO_PAGE_SIZE,
                           RELEVO_PAGE_HOST);
        break;
    default:
        break;
    }
}

static uint32_t host_read(void *ctx, uint32_t addr, unsigned size)
{
    machine *m = ctx;
    uint32_t v = (addr * 0x9E3779B1u) ^ size;
    log_access(m, LOG_READ, addr, v, size);
    host_effects(m, addr, v);
    return v;
}

static void host_write(void *ctx, uint32_t addr, uint32_t v, unsigned size)
{
    machine *m = ctx;
    log_access(m, LOG_WRITE, addr, v, size);
    host_effects(m, addr, v);
}

/* Line A and line F: what the host does depends on the opcode. It may
 * ask for the exception, handle the call (changing registers and memory
 * through the API, as a native call does) or stop the run. */
static relevo_trap_result host_line(machine *m, relevo_cpu *cpu,
                                    uint16_t op, unsigned kind)
{
    log_access(m, kind, m->api->relevo_get_regs(cpu)->pc, op, 0);
    relevo_regs *r = m->api->relevo_get_regs(cpu);
    switch ((op ^ op >> 5) & 3) {
    case 0:
        return RELEVO_TRAP_EXCEPTION;
    case 1:
        r->d[0] += op;
        r->a[0] ^= (uint32_t)op << 4;
        return RELEVO_TRAP_HANDLED;
    case 2: {
        uint32_t a = WIN + (op & 0x7FE);
        uint32_t v = m->api->relevo_read(cpu, a, 4);
        m->api->relevo_write(cpu, a, v + op, 4);
        r->d[1] = v;
        return RELEVO_TRAP_HANDLED;
    }
    default:
        return RELEVO_TRAP_STOP;
    }
}

static relevo_trap_result host_line_a(void *ctx, relevo_cpu *cpu,
                                      uint16_t op)
{
    machine *m = ctx;
    if ((op & 0x0F00) == 0x0F00)
        return RELEVO_TRAP_EXCEPTION;   /* no host function: the vector */
    return host_line(m, cpu, op, LOG_LINE_A);
}

static relevo_trap_result host_line_f(void *ctx, relevo_cpu *cpu,
                                      uint16_t op)
{
    return host_line(ctx, cpu, op, LOG_LINE_F);
}

static void host_reset(void *ctx)
{
    log_access(ctx, LOG_RESET, 0, 0, 0);
}

static relevo_fpu fpu = RELEVO_FPU_NONE;

/* Returns false if this library does not run that configuration. */
static bool machine_init(machine *m, const api *a)
{
    memset(m, 0, sizeof *m);
    m->api = a;
    m->mem = calloc(1, MEM_SIZE);
    if (m->mem == NULL) {
        fprintf(stderr, "crdiff: out of memory\n");
        exit(2);
    }
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.fpu = fpu;
    cfg.engine = RELEVO_ENGINE_INTERP;
    cfg.memory = m->mem;
    cfg.memory_size = MEM_SIZE;
    cfg.host.ctx = m;
    cfg.host.read = host_read;
    cfg.host.write = host_write;
    cfg.host.line_a = host_line_a;
    cfg.host.line_f = host_line_f;
    cfg.host.reset = host_reset;
    m->cpu = a->relevo_create(&cfg);
    if (m->cpu == NULL) {
        free(m->mem);
        return false;
    }
    a->relevo_map(m->cpu, ROM_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_ROM);
    a->relevo_map(m->cpu, HOST_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST);
    a->relevo_map(m->cpu, NONE_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_NONE);
    return true;
}

static void machine_free(machine *m)
{
    m->api->relevo_destroy(m->cpu);
    free(m->mem);
}

/* ---- What the 68010 to 68060 and the FPU add -------------------------- */

#define VBR2 0x6000u            /* a second vector table */

/* FPU opmodes of the transcendental functions (68881/68882). */
static bool transcendental(unsigned opmode)
{
    static const uint8_t t[] = { 0x02, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D,
                                 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x15,
                                 0x16, 0x19, 0x1C, 0x1D };
    if (opmode >= 0x30 && opmode <= 0x37)
        return true;        /* FSINCOS */
    for (unsigned i = 0; i < sizeof t; i++)
        if (t[i] == opmode)
            return true;
    return false;
}

static unsigned rnd_trans_op(void)
{
    static const uint8_t t[] = { 0x02, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D,
                                 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x15,
                                 0x16, 0x19, 0x1C, 0x1D, 0x30 };
    unsigned op = t[rn(sizeof t)];
    return op == 0x30 ? op | rn(8) : op;
}

/* A control register number for MOVEC: those of some model, sometimes
 * any. */
static unsigned rnd_ctrl_reg(void)
{
    static const uint16_t regs[] = { 0x000, 0x001, 0x002, 0x003, 0x004,
                                     0x005, 0x006, 0x007, 0x008, 0x800,
                                     0x801, 0x802, 0x803, 0x804, 0x805,
                                     0x806, 0x807, 0x808 };
    if (chance(5))
        return rnd() & 0xFFF;
    return regs[rn(sizeof regs / sizeof regs[0])];
}

static void rnd_fpu_more(uint32_t pc, unsigned dn)
{
    unsigned r = rn(8);
    switch (rn(14)) {
    case 0: case 12:    /* a transcendental function, FPm,FPn or <ea>,FPn */
        if (chance(40)) {
            w16(0xF200);
            w16(rn(8) << 10 | r << 7 | rnd_trans_op());
        } else {
            static const uint8_t fmts[] = { 0, 1, 4, 5, 6, 2 };
            static const unsigned fsz[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };
            unsigned fmt = fmts[rn(sizeof fmts)];
            w16(0);
            w16(0x4000 | fmt << 10 | r << 7 | rnd_trans_op());
            unsigned sz = fsz[fmt];
            unsigned ea = rnd_ea(sz <= 4 ? C_DATA : C_DATA & ~(C_DREG | C_IMM),
                                 sz == 1 ? 1 : sz == 2 ? 2 : 4);
            words[0] = (uint16_t)(0xF200 | ea);
        }
        break;
    case 1: {   /* FMOVEM data registers */
        bool to_mem = chance(50);
        unsigned mode = rn(4);      /* static/dynamic, predec/postinc */
        w16(0);
        w16((to_mem ? 0xE000 : 0xC000) | mode << 11
            | ((mode & 1) ? dn << 4 : rnd() & 0xFF));
        unsigned ea = rnd_ea(to_mem ? (C_CONTROL & ~(C_PCD16 | C_PCD8)) | C_PRE
                                    : C_CONTROL | C_POST, 4);
        words[0] = (uint16_t)(0xF200 | ea);
        break;
    }
    case 2: {   /* FMOVEM / FMOVE control registers */
        bool to_mem = chance(50);
        unsigned list = 1 + rn(7);
        w16(0);
        w16((to_mem ? 0xA000 : 0x8000) | list << 10);
        unsigned single = list == 1 || list == 2 || list == 4;
        unsigned classes = to_mem ? (single ? C_ALT : C_MEMALT)
                                  : (single ? C_ALL : C_DATA & ~C_DREG);
        if (list == 1 && !chance(10))
            classes &= ~C_AREG;     /* only FPIAR takes An */
        unsigned ea = rnd_ea(classes, 4);
        if (!to_mem && (ea >> 3) == 7 && (ea & 7) == 4 && !single) {
            /* #imm with several registers: one long each */
            for (unsigned i = 1; i < (list & 1) + (list >> 1 & 1) + (list >> 2); i++)
                w32(rnd_value());
        }
        words[0] = (uint16_t)(0xF200 | ea);
        break;
    }
    case 3:     /* FMOVECR */
        w16(0xF200);
        w16(0x5C00 | r << 7 | (chance(80) ? rn(0x40) : rn(0x80)));
        break;
    case 4: {   /* FSAVE, FRESTORE */
        bool save = chance(50);
        w16(0);
        unsigned ea = rnd_ea(save ? (C_CONTROL & ~(C_PCD16 | C_PCD8)) | C_PRE
                                  : C_CONTROL | C_POST, 4);
        words[0] = (uint16_t)((save ? 0xF300 : 0xF340) | ea);
        break;
    }
    case 5: {   /* FTRAPcc */
        unsigned k = 2 + rn(3);
        w16(0xF278 | k);
        w16(chance(95) ? rn(0x20) : rn(0x40));
        if (k == 2)
            w16(rnd());
        else if (k == 3)
            w32(rnd());
        break;
    }
    case 6:     /* FDBcc */
        w16(0xF248 | dn);
        w16(chance(95) ? rn(0x20) : rn(0x40));
        w16((uint32_t)rnd_disp(pc + 2));
        break;
    case 7: {   /* FScc <ea> */
        w16(0);
        w16(rn(0x20));
        words[0] = (uint16_t)(0xF240 | rnd_ea(C_DATAALT, 1));
        break;
    }
    case 8: {   /* FMOVE.P out, static or dynamic k */
        bool dyn = chance(40);
        w16(0);
        w16(0x6000 | (dyn ? 7 : 3) << 10 | r << 7
            | (dyn ? dn << 4 : (chance(70) ? rn(18) : rn(0x80))));
        words[0] = (uint16_t)(0xF200 | rnd_ea(C_MEMALT, 4));
        break;
    }
    case 9:     /* FNOP */
        w16(0xF280);
        w16(0);
        break;
    case 10: {  /* any general FPU command word */
        w16(0xF200 | (chance(50) ? 0 : rn(0x40)));
        w16(rnd());
        w32(rnd_value());
        w32(rnd_value());
        break;
    }
    case 11: {  /* FMOVE FPCR, an exception enabled: what follows traps */
        w16(0xF23C);
        w16(0x9000);
        w32((rnd() & 0xFF00) | (rnd() & 0xF0));
        break;
    }
    default:
        rnd_fpu(pc, dn);
        break;
    }
}

/* One instruction of the 68010 and later, or of the 68000 that changes
 * the machine state (SR, STOP, RESET, traps). */
static void rnd_insn_rr2(uint32_t pc)
{
    unsigned dn = rn(8), dy = rn(8), size = rnd_size();
    bool m020 = model >= RELEVO_68020;
    switch (rn(30)) {
    case 0: case 1:     /* MOVEC */
        w16(0x4E7A | rn(2));
        w16(rn(16) << 12 | rnd_ctrl_reg());
        break;
    case 2: {   /* MOVES */
        w16(0);
        w16(rn(16) << 12 | rn(2) << 11 | (chance(3) ? rnd() & 0x7FF : 0));
        unsigned ea = rnd_ea(C_MEMALT, size);
        words[0] = (uint16_t)(0x0E00 | size_bits(size) << 6 | ea);
        break;
    }
    case 3:     /* RTD */
        w16(0x4E74);
        w16(chance(70) ? rn(32) * 2 : rnd());
        break;
    case 4: case 5:     /* RTE: of whatever frame the stack holds */
        w16(0x4E73);
        break;
    case 6: {   /* MOVE from SR, CCR; to SR, CCR */
        static const uint16_t op[] = { 0x40C0, 0x42C0, 0x46C0, 0x44C0 };
        unsigned k = rn(4);
        w16(0);
        unsigned ea = rnd_ea(k < 2 ? C_DATAALT : C_DATA, 2);
        words[0] = (uint16_t)(op[k] | ea);
        break;
    }
    case 7: {   /* ORI ANDI EORI to SR and CCR */
        static const uint16_t op[] = { 0x007C, 0x027C, 0x0A7C, 0x003C,
                                       0x023C, 0x0A3C };
        w16(op[rn(6)]);
        w16(rnd() & (chance(50) ? 0xFFFF : 0x00FF));
        break;
    }
    case 8:     /* STOP, RESET */
        if (chance(50)) {
            w16(0x4E72);
            w16((rnd() & 0xF71F) | (chance(80) ? 0x2000 : 0));
        } else {
            w16(0x4E70);
        }
        break;
    case 9: {   /* TRAP, TRAPV, ILLEGAL, BKPT, line A, MOVE USP */
        static const uint16_t op[] = { 0x4E40, 0x4E76, 0x4AFC, 0x4848,
                                       0xA000, 0x4E60 };
        unsigned k = rn(6);
        w16(op[k] | (k == 0 ? rn(16) : k == 3 ? rn(8) : k == 4 ? rnd() & 0xFFF
                     : k == 5 ? rn(16) : 0));
        break;
    }
    case 10: {  /* CAS2 */
        if (!m020)
            goto plain;
        w16(chance(50) ? 0x0CFC : 0x0EFC);
        w16(rn(16) << 12 | rn(8) << 6 | rn(8));
        w16(rn(16) << 12 | rn(8) << 6 | rn(8));
        break;
    }
    case 11: {  /* CHK2, CMP2 */
        if (!m020)
            goto plain;
        w16(0);
        w16(rn(16) << 12 | rn(2) << 11);
        unsigned ea = rnd_ea(C_CONTROL, size);
        words[0] = (uint16_t)(0x00C0 | size_bits(size) << 9 | ea);
        break;
    }
    case 12: {  /* TRAPcc */
        if (!m020)
            goto plain;
        unsigned k = 2 + rn(3);
        w16(0x50F8 | rn(16) << 8 | k);
        if (k == 2)
            w16(rnd());
        else if (k == 3)
            w32(rnd());
        break;
    }
    case 13:    /* PACK, UNPK -(Ax),-(Ay); CALLM, RTM */
        if (!m020)
            goto plain;
        if (chance(80)) {
            w16((chance(50) ? 0x8148 : 0x8188) | dn << 9 | dy);
            w16(rnd());
        } else {
            w16(chance(50) ? 0x06C0 | rn(64) : 0x06C0 | rn(16));
            w16(rnd());
        }
        break;
    case 14: {  /* MOVE16 (68040 on) */
        if (chance(50)) {
            w16(0xF620 | dy);
            w16(0x8000 | dn << 12);
        } else {
            w16(0xF600 | rn(4) << 3 | dy);
            w32(rnd_addr());
        }
        break;
    }
    case 15:    /* CINV, CPUSH */
        w16(0xF400 | rn(4) << 6 | rn(2) << 5 | rn(4) << 3 | dy);
        break;
    case 16: case 17: case 18: case 19: case 20: case 21:
        if (fpu != RELEVO_FPU_NONE || chance(10)) {
            rnd_fpu_more(pc, dn);
            break;
        }
        goto plain;
    case 22: {  /* MULx.L, DIVx.L with the 64-bit forms */
        if (!m020)
            goto plain;
        bool div = chance(50);
        w16(0);
        w16(rn(8) << 12 | rn(2) << 11 | rn(2) << 10 | rn(8));
        unsigned ea = rnd_ea(C_DATA, 4);
        words[0] = (uint16_t)((div ? 0x4C40 : 0x4C00) | ea);
        break;
    }
    case 23: {  /* bit fields in memory, offset and width in registers */
        if (!m020)
            goto plain;
        unsigned t = rn(8);
        bool ro = t == 0 || t == 1 || t == 3 || t == 5;
        w16(0);
        w16(rn(8) << 12 | 0x0800 | rn(8) << 6 | 0x20 | rn(8));
        unsigned ea = rnd_ea(ro ? C_CONTROL | C_DREG
                                : (C_CONTROL & ~(C_PCD16 | C_PCD8)) | C_DREG, 1);
        words[0] = (uint16_t)(0xE8C0 | t << 8 | ea);
        break;
    }
    default:
    plain:
        rnd_insn(pc);
        return;
    }
}

/* ---- A case ----------------------------------------------------------- */

static unsigned garbage = 10;   /* percent */
static unsigned ulps = 2;
static bool strict;
static unsigned long cases, steps;
static unsigned long trans_steps, trans_differ, trans_max_ulps;

typedef struct state {
    relevo_regs r;
    unsigned ipl;
} state;

/* Every field of relevo_regs, for comparing and printing. */
#define REG32(X)                                                            \
    X(d[0]) X(d[1]) X(d[2]) X(d[3]) X(d[4]) X(d[5]) X(d[6]) X(d[7])         \
    X(a[0]) X(a[1]) X(a[2]) X(a[3]) X(a[4]) X(a[5]) X(a[6]) X(a[7])         \
    X(pc) X(usp) X(isp) X(msp) X(vbr) X(cacr) X(caar) X(sfc) X(dfc)         \
    X(fpcr) X(fpsr) X(fpiar) X(tc) X(itt0) X(itt1) X(dtt0) X(dtt1) X(mmusr) \
    X(urp) X(srp) X(buscr) X(pcr)

static void load_state(machine *m, const state *s)
{
    m->api->relevo_reset(m->cpu);   /* out of a double bus fault */
    relevo_regs *r = m->api->relevo_get_regs(m->cpu);
#define LOAD(f) r->f = s->r.f;
    REG32(LOAD)
#undef LOAD
    r->sr = s->r.sr;
    for (int i = 0; i < 8; i++)
        r->fp[i] = s->r.fp[i];
    m->api->relevo_set_ipl(m->cpu, s->ipl);
    m->nlog = 0;
}

static bool fp_equal(relevo_fpreg a, relevo_fpreg b)
{
    return a.sign_exponent == b.sign_exponent && a.mantissa == b.mantissa;
}

/* Units in the last place between two finite extended values of the same
 * sign (UINT64_MAX if too far apart or not comparable). */
static uint64_t ulp_distance(relevo_fpreg a, relevo_fpreg b)
{
    if ((a.sign_exponent ^ b.sign_exponent) & 0x8000)
        return UINT64_MAX;
    unsigned ea = a.sign_exponent & 0x7FFF, eb = b.sign_exponent & 0x7FFF;
    if (ea == 0x7FFF || eb == 0x7FFF)
        return fp_equal(a, b) ? 0 : UINT64_MAX;
    if (ea > eb || (ea == eb && a.mantissa > b.mantissa)) {
        relevo_fpreg t = a;
        a = b;
        b = t;
        unsigned te = ea;
        ea = eb;
        eb = te;
    }
    if (ea == eb)
        return b.mantissa - a.mantissa;
    if (eb != ea + 1)
        return UINT64_MAX;
    if (ea == 0)        /* denormal to the smallest normal */
        return b.mantissa - a.mantissa;
    return (0x8000000000000000ull - (a.mantissa - 0x8000000000000000ull))
         + (b.mantissa - 0x8000000000000000ull);
}

static void print_fp(const char *name, relevo_fpreg f)
{
    printf("%s=%04X:%016" PRIX64, name, f.sign_exponent, f.mantissa);
}

/* Compares the two CPUs; prints what differs (with print). `tol_fp`: the
 * FPU data registers may differ by `ulps` (the instruction was a
 * transcendental function). */
static bool compare_cpus(machine *x, machine *y, relevo_status sx,
                         relevo_status sy, bool tol_fp, bool print)
{
    relevo_regs *a = x->api->relevo_get_regs(x->cpu);
    relevo_regs *b = y->api->relevo_get_regs(y->cpu);
    bool ok = true;
#define CMP(f)                                                              \
    if (a->f != b->f) {                                                     \
        if (print)                                                          \
            printf("  %-6s Rust %08" PRIX32 "  C %08" PRIX32 "\n", #f,      \
                   (uint32_t)a->f, (uint32_t)b->f);                         \
        ok = false;                                                         \
    }
    REG32(CMP)
    CMP(sr)
#undef CMP
    bool fp_tolerated = false;
    for (int i = 0; i < 8; i++) {
        if (fp_equal(a->fp[i], b->fp[i]))
            continue;
        uint64_t u = ulp_distance(a->fp[i], b->fp[i]);
        if (tol_fp && !strict && u <= ulps) {
            fp_tolerated = true;
            if (u > trans_max_ulps)
                trans_max_ulps = u;
            continue;
        }
        if (print) {
            char n[8];
            snprintf(n, sizeof n, "fp%d", i);
            printf("  ");
            print_fp(n, a->fp[i]);
            printf(" (Rust)  ");
            print_fp(n, b->fp[i]);
            printf(" (C)");
            if (u != UINT64_MAX)
                printf("  %" PRIu64 " ulps", u);
            printf("\n");
        }
        ok = false;
    }
    if (sx != sy) {
        if (print)
            printf("  status Rust %d  C %d\n", (int)sx, (int)sy);
        ok = false;
    }
    uint64_t ix = x->api->relevo_instructions(x->cpu);
    uint64_t iy = y->api->relevo_instructions(y->cpu);
    if (ix != iy) {
        if (print)
            printf("  instructions Rust %" PRIu64 "  C %" PRIu64 "\n", ix, iy);
        ok = false;
    }
    uint64_t cx = x->api->relevo_cycles(x->cpu);
    uint64_t cy = y->api->relevo_cycles(y->cpu);
    if (cx != cy) {
        if (print)
            printf("  cycles Rust %" PRIu64 "  C %" PRIu64 "\n", cx, cy);
        ok = false;
    }
    if (x->nlog != y->nlog
        || memcmp(x->log, y->log, x->nlog * sizeof x->log[0]) != 0) {
        if (print) {
            static const char *const kind[] = { "read", "write", "line A",
                                                "line F", "reset" };
            printf("  host calls differ (Rust %u, C %u):\n", x->nlog, y->nlog);
            for (unsigned i = 0; i < x->nlog || i < y->nlog; i++) {
                for (int k = 0; k < 2; k++) {
                    machine *m = k ? y : x;
                    if (i >= m->nlog)
                        continue;
                    access *l = &m->log[i];
                    printf("    %s %u: %s %08" PRIX32 " %08" PRIX32 " %u\n",
                           k ? "C   " : "Rust", i, kind[l->kind], l->addr,
                           l->value, l->size);
                }
            }
        }
        ok = false;
    }
    if (ok && fp_tolerated) {
        /* The rest of the case compares exactly. */
        trans_differ++;
        for (int i = 0; i < 8; i++)
            a->fp[i] = b->fp[i];
    }
    return ok;
}

/* The memory the cases reach (all of it with `full`). */
static bool compare_mem(machine *x, machine *y, bool full, bool print)
{
    static const uint32_t win[][2] = {
        { 0, 0x8000 }, { CODE, CODE_END }, { WIN - 0x10000, WIN + 0x10000 },
    };
    for (unsigned w = 0; w < (full ? 1u : 3u); w++) {
        uint32_t lo = full ? 0 : win[w][0];
        uint32_t hi = full ? MEM_SIZE : win[w][1];
        if (memcmp(x->mem + lo, y->mem + lo, hi - lo) == 0)
            continue;
        for (uint32_t i = lo; i < hi && print; i++)
            if (x->mem[i] != y->mem[i]) {
                printf("  memory %06" PRIX32 ": Rust %02X  C %02X\n", i,
                       x->mem[i], y->mem[i]);
                break;
            }
        return false;
    }
    return true;
}

/* Sets a memory back to init_mem where a case may have changed it. */
static void restore(machine *m, bool full)
{
    if (full) {
        memcpy(m->mem, init_mem, MEM_SIZE);
        return;
    }
    memcpy(m->mem, init_mem, 0x8000);
    memcpy(m->mem + CODE, init_mem + CODE, CODE_END - CODE);
    memcpy(m->mem + WIN - 0x10000, init_mem + WIN - 0x10000, 0x20000);
}

/* Whether the instruction at pc is a transcendental function the FPU
 * computes (68881 and 68882: the 68040 and 68060 leave them to software). */
static bool trans_at(const machine *m, uint32_t pc)
{
    if (fpu != RELEVO_FPU_68881 && fpu != RELEVO_FPU_68882)
        return false;
    if (model < RELEVO_68020)
        return false;
    if (pc > MEM_SIZE - 4 || (pc >= HOST_PAGE && pc < NONE_PAGE + RELEVO_PAGE_SIZE))
        return false;
    uint32_t op = (uint32_t)m->mem[pc] << 8 | m->mem[pc + 1];
    uint32_t cmd = (uint32_t)m->mem[pc + 2] << 8 | m->mem[pc + 3];
    if ((op & 0xFFC0) != 0xF200)
        return false;
    /* FPm,FPn (000) or <ea>,FPn (010) */
    if ((cmd & 0xA000) != 0)
        return false;
    return transcendental(cmd & 0x7F);
}

static void print_state(const state *s)
{
    for (int i = 0; i < 8; i++)
        printf("  D%d=%08" PRIX32 " A%d=%08" PRIX32 "\n", i, s->r.d[i], i,
               s->r.a[i]);
    printf("  PC=%08" PRIX32 " SR=%04X USP=%08" PRIX32 " ISP=%08" PRIX32
           " MSP=%08" PRIX32 " VBR=%08" PRIX32 " IPL=%u\n", s->r.pc, s->r.sr,
           s->r.usp, s->r.isp, s->r.msp, s->r.vbr, s->ipl);
    printf("  CACR=%08" PRIX32 " CAAR=%08" PRIX32 " SFC=%" PRIX32 " DFC=%"
           PRIX32 " FPCR=%08" PRIX32 " FPSR=%08" PRIX32 " FPIAR=%08" PRIX32
           "\n", s->r.cacr, s->r.caar, s->r.sfc, s->r.dfc, s->r.fpcr,
           s->r.fpsr, s->r.fpiar);
    for (int i = 0; i < 8; i++) {
        char n[8];
        snprintf(n, sizeof n, "  FP%d", i);
        print_fp(n, s->r.fp[i]);
        printf(i % 2 ? "\n" : "");
    }
}

static const char *model_name(relevo_model m)
{
    static const char *const n[] = { "68000", "68010", "68020", "68030",
                                     "68040", "68060" };
    return n[m];
}

static const char *fpu_name(relevo_fpu f)
{
    static const char *const n[] = { "no FPU", "68881", "68882",
                                     "internal FPU" };
    return n[f];
}

static uint64_t seed;

static bool run_case(machine *x, machine *y, unsigned maxlen, bool full_mem)
{
    /* Data and code. */
    for (uint32_t a = WIN - 0x100; a < WIN + WIN_SIZE + 0x100; a += 4)
        put32(init_mem, a, chance(30) ? rnd_addr() : rnd_value());
    unsigned len = 1 + rn(maxlen);
    uint32_t start = CODE + (chance(50) ? 0 : (rn(0x800) & ~1u));
    uint32_t pc = start;
    for (unsigned i = 0; i < len; i++) {
        unsigned g = rn(100);
        if (g < garbage) {
            nwords = 0;
            unsigned n = 1 + rn(4);
            for (unsigned k = 0; k < n; k++)
                w16(chance(20) ? (0xF000 | (rnd() & 0x0FFF)) : rnd());
        } else if (g < garbage + (100 - garbage) / 3) {
            nwords = 0;
            rnd_insn_rr2(pc);
        } else {
            rnd_insn(pc);
        }
        for (unsigned k = 0; k < nwords && pc + 2 <= CODE_END; k++, pc += 2)
            put16(init_mem, pc, words[k]);
    }
    /* What follows: random words, then NOPs */
    for (uint32_t a = pc; a < pc + 16 && a + 2 <= CODE_END; a += 2)
        put16(init_mem, a, chance(50) ? 0x4E71 : rnd());

    state s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 8; i++) {
        s.r.d[i] = rnd_value();
        s.r.a[i] = chance(85) ? rnd_addr() : rnd_value();
    }
    bool m_bits = model >= RELEVO_68020 && model <= RELEVO_68040;
    uint16_t sr = (uint16_t)(rn(8) << 8 | (rnd() & 0x1F));
    if (chance(80))
        sr |= 0x2000;
    if (chance(3))
        sr |= 0x8000;                           /* T1 */
    if (m_bits && chance(3))
        sr |= 0x4000;                           /* T0 */
    if (m_bits && chance(20))
        sr |= 0x1000;                           /* M */
    s.r.sr = sr;
    s.r.a[7] = WIN + (rn(WIN_SIZE) & ~1u);
    if (chance(5))
        s.r.a[7] = rnd_addr();
    s.r.usp = WIN + (rn(WIN_SIZE) & ~1u);
    s.r.isp = WIN + (rn(WIN_SIZE) & ~1u);
    s.r.msp = WIN + (rn(WIN_SIZE) & ~1u);
    s.r.pc = start;
    s.r.vbr = chance(70) ? 0 : chance(70) ? VBR2 : rnd_addr();
    s.r.cacr = chance(50) ? 0 : rnd();
    s.r.caar = rnd_addr();
    s.r.sfc = chance(80) ? rn(8) : rnd();
    s.r.dfc = chance(80) ? rn(8) : rnd();
    s.r.tc = chance(70) ? 0 : rnd();
    s.r.itt0 = rnd();
    s.r.itt1 = rnd();
    s.r.dtt0 = rnd();
    s.r.dtt1 = rnd();
    s.r.mmusr = rnd();
    s.r.urp = rnd();
    s.r.srp = rnd();
    s.r.buscr = rnd();
    s.r.pcr = rnd();
    for (int i = 0; i < 8; i++)
        s.r.fp[i] = rnd_fp();
    /* rounding and precision; exceptions enabled now and then */
    s.r.fpcr = rnd() & 0xF0;
    if (chance(15))
        s.r.fpcr |= rnd() & 0xFF00;
    s.r.fpsr = rnd() & 0x0FFFFFF8u;
    s.r.fpiar = rnd();
    s.ipl = chance(80) ? 0 : rn(8);

    restore(x, full_mem);
    restore(y, full_mem);
    load_state(x, &s);
    load_state(y, &s);
    uint64_t budget = len + (chance(30) ? rn(20) : 0);
    cases++;
    bool ok = true;
    for (uint64_t i = 0; i < budget; i++) {
        if (chance(3)) {
            unsigned ipl = rn(8);
            x->api->relevo_set_ipl(x->cpu, ipl);
            y->api->relevo_set_ipl(y->cpu, ipl);
        }
        uint32_t ipc = y->api->relevo_get_regs(y->cpu)->pc;
        bool trans = trans_at(y, ipc);
        relevo_status sx = x->api->relevo_run(x->cpu, 1);
        relevo_status sy = y->api->relevo_run(y->cpu, 1);
        steps++;
        trans_steps += trans;
        if (!compare_cpus(x, y, sx, sy, trans, false)) {
            printf("crdiff %s, %s, seed 0x%" PRIX64 ": case %lu differs at"
                   " instruction %" PRIu64 " (PC %08" PRIX32 ":",
                   model_name(model), fpu_name(fpu), seed, cases, i + 1, ipc);
            if (ipc <= MEM_SIZE - 10)
                for (uint32_t a = ipc; a < ipc + 10; a += 2)
                    printf(" %04X", (unsigned)(y->mem[a] << 8 | y->mem[a + 1]));
            printf(")%s\n", trans ? ", a transcendental function" : "");
            compare_cpus(x, y, sx, sy, trans, true);
            ok = false;
            break;
        }
        if (sx != RELEVO_BUDGET)
            break;
    }
    if (ok && !compare_mem(x, y, full_mem, false)) {
        printf("crdiff %s, %s, seed 0x%" PRIX64 ": case %lu differs in"
               " memory\n", model_name(model), fpu_name(fpu), seed, cases);
        compare_mem(x, y, full_mem, true);
        ok = false;
    }
    if (!ok) {
        printf("  code at %06" PRIX32 ":", start);
        for (uint32_t a = start; a < pc + 16 && a + 2 <= CODE_END; a += 2)
            printf(" %04X", (unsigned)(init_mem[a] << 8 | init_mem[a + 1]));
        printf("\n  budget %" PRIu64 ", state before:\n", budget);
        print_state(&s);
    }
    return ok;
}

/* Runs `n` cases on one configuration; 0 if equal, 1 if not, -1 if the
 * Rust library does not run it. */
static int run_config(unsigned long n, unsigned maxlen)
{
    machine x, y;
    if (!machine_init(&y, &c_api)) {
        fprintf(stderr, "crdiff: the C library refuses the %s with %s\n",
                model_name(model), fpu_name(fpu));
        exit(2);
    }
    if (!machine_init(&x, &rust_api)) {
        printf("crdiff %s, %s: SKIPPED, the Rust library does not run it"
               " yet\n", model_name(model), fpu_name(fpu));
        machine_free(&y);
        return -1;
    }
    gen_fpu = fpu;
    cases = steps = trans_steps = trans_differ = trans_max_ulps = 0;
    restore(&x, true);
    restore(&y, true);
    int result = 0;
    for (unsigned long i = 0; i < n; i++)
        if (!run_case(&x, &y, maxlen, (i & 255) == 255)) {
            result = 1;
            break;
        }
    if (result == 0) {
        printf("crdiff %s, %s: %lu cases of up to %u instructions (%lu run),"
               " all equal", model_name(model), fpu_name(fpu), cases, maxlen,
               steps);
        if (trans_steps != 0)
            printf("; %lu transcendental, %lu of them within %u ulps (max %lu)",
                   trans_steps, trans_differ, ulps, trans_max_ulps);
        printf("\n");
    }
    machine_free(&x);
    machine_free(&y);
    return result;
}

static relevo_fpu default_fpu(relevo_model m)
{
    if (m == RELEVO_68020 || m == RELEVO_68030)
        return RELEVO_FPU_68882;
    if (m >= RELEVO_68040)
        return RELEVO_FPU_INTERNAL;
    return RELEVO_FPU_NONE;
}

int main(int argc, char **argv)
{
    unsigned long n = 20000;
    unsigned maxlen = 8;
    bool all = false;
    int fpu_arg = -1;
    seed = rng_state;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "-m") == 0 && v != NULL) {
            long m = strtol(v, NULL, 10);
            model = m == 68010 ? RELEVO_68010 : m == 68020 ? RELEVO_68020
                  : m == 68030 ? RELEVO_68030 : m == 68040 ? RELEVO_68040
                  : m == 68060 ? RELEVO_68060 : RELEVO_68000;
            i++;
        } else if (strcmp(argv[i], "-f") == 0 && v != NULL) {
            fpu_arg = strcmp(v, "68881") == 0 ? RELEVO_FPU_68881
                    : strcmp(v, "68882") == 0 ? RELEVO_FPU_68882
                    : strcmp(v, "internal") == 0 ? RELEVO_FPU_INTERNAL
                    : RELEVO_FPU_NONE;
            i++;
        } else if (strcmp(argv[i], "-n") == 0 && v != NULL) {
            n = strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(argv[i], "-k") == 0 && v != NULL) {
            maxlen = (unsigned)strtoul(v, NULL, 10);
            if (maxlen == 0)
                maxlen = 1;
            i++;
        } else if (strcmp(argv[i], "-s") == 0 && v != NULL) {
            rng_state = strtoull(v, NULL, 0) | 1;
            seed = rng_state;
            i++;
        } else if (strcmp(argv[i], "-g") == 0 && v != NULL) {
            garbage = (unsigned)strtoul(v, NULL, 10);
            if (garbage > 100)
                garbage = 100;
            i++;
        } else if (strcmp(argv[i], "-u") == 0 && v != NULL) {
            ulps = (unsigned)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(argv[i], "-T") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            all = true;
        } else {
            fprintf(stderr, "usage: crdiff [-m MODEL] [-f none|68881|68882|"
                            "internal] [-a] [-n CASES] [-k MAXLEN] [-s SEED]"
                            " [-g PERCENT] [-u ULPS] [-T]\n");
            return 2;
        }
    }
    setup_static();
    /* A second vector table, for a VBR that is not 0 */
    for (uint32_t v = 0; v < 256; v++)
        put32(init_mem, VBR2 + v * 4, HANDLER + 2 * (v & 15));

    if (!all) {
        fpu = fpu_arg >= 0 ? (relevo_fpu)fpu_arg : default_fpu(model);
        return run_config(n, maxlen) > 0;
    }
    static const struct { relevo_model m; relevo_fpu f; } configs[] = {
        { RELEVO_68000, RELEVO_FPU_NONE }, { RELEVO_68010, RELEVO_FPU_NONE },
        { RELEVO_68020, RELEVO_FPU_NONE }, { RELEVO_68020, RELEVO_FPU_68881 },
        { RELEVO_68020, RELEVO_FPU_68882 }, { RELEVO_68030, RELEVO_FPU_NONE },
        { RELEVO_68030, RELEVO_FPU_68882 }, { RELEVO_68040, RELEVO_FPU_NONE },
        { RELEVO_68040, RELEVO_FPU_INTERNAL }, { RELEVO_68060, RELEVO_FPU_NONE },
        { RELEVO_68060, RELEVO_FPU_INTERNAL },
    };
    for (unsigned i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        model = configs[i].m;
        fpu = configs[i].f;
        if (run_config(n, maxlen) > 0)
            return 1;
    }
    return 0;
}
