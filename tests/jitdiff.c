/* SPDX-License-Identifier: MPL-2.0
 *
 * Differential test of the JIT against the interpreter.
 *
 *   jitdiff [-m 68000|68010|68020|68040] [-n CASES] [-k MAXLEN] [-s SEED]
 *           [-x] [-e]
 *
 * Each case writes 1 to MAXLEN random instructions (mostly of the kinds the
 * JIT translates, with random operands, the 68020's full extension word
 * included, and some it does not; operands also point into the code), random
 * registers, flags and data, and runs them on two CPUs, one per engine,
 * (from the 68020 on with an FPU: a 68882, or the 68040's own; random FPU
 * registers, FPCR and FPSR, and FPU instructions too),
 * with identical memories, for as many instructions as were written (the
 * flow may go anywhere: exceptions, random jumps, data run as code). Then
 * it compares registers, SR, PC, the FPU, the instruction count, the run
 * status,
 * the accesses to a host page and memory (the windows the operands
 * reach every case, all 16 MB every 256 cases). -x makes the JIT use
 * BSWAP instead of MOVBE; -e turns the condition codes into CCR bits at
 * every exit instead of keeping them in x86 form. -k 1 tests one instruction at a time. Exits
 * with 1 at the first difference, which it prints.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relevo.h"
#include "jit/jit.h"
#include "gen68k.h"

/* ---- The two machines ------------------------------------------------- */

#define LOG_MAX 64

typedef struct access {
    uint32_t addr, value;
    unsigned size;
    bool write;
} access;

typedef struct machine {
    relevo_cpu *cpu;
    uint8_t *mem;
    access log[LOG_MAX];
    unsigned nlog;
} machine;

static void host_effects(machine *m, uint32_t addr, uint32_t v);

static uint32_t host_read(void *ctx, uint32_t addr, unsigned size)
{
    machine *m = ctx;
    uint32_t v = (addr * 0x9E3779B1u) ^ size;
    if (m->nlog < LOG_MAX)
        m->log[m->nlog++] = (access){ addr, v, size, false };
    host_effects(m, addr, v);
    return v;
}

static void host_write(void *ctx, uint32_t addr, uint32_t v, unsigned size)
{
    machine *m = ctx;
    if (m->nlog < LOG_MAX)
        m->log[m->nlog++] = (access){ addr, v, size, true };
    host_effects(m, addr, v);
}

static relevo_fpu model_fpu(void)
{
    if (model == RELEVO_68020 || model == RELEVO_68030)
        return RELEVO_FPU_68882;
    if (model == RELEVO_68040)
        return RELEVO_FPU_INTERNAL;
    return RELEVO_FPU_NONE;
}

static bool machine_init(machine *m, relevo_engine engine)
{
    memset(m, 0, sizeof *m);
    m->mem = calloc(1, MEM_SIZE);
    if (m->mem == NULL)
        return false;
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.fpu = model_fpu();
    cfg.engine = engine;
    cfg.memory = m->mem;
    cfg.memory_size = MEM_SIZE;
    cfg.host.ctx = m;
    cfg.host.read = host_read;
    cfg.host.write = host_write;
    m->cpu = relevo_create(&cfg);
    if (m->cpu == NULL)
        return false;
    relevo_map(m->cpu, ROM_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_ROM);
    relevo_map(m->cpu, HOST_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST);
    relevo_map(m->cpu, NONE_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_NONE);
    return true;
}

/* Some host accesses change the translations under a running block, as a
 * host may: code patched (and invalidated), an invalidation, a remap of
 * a page as it was (which flushes everything). */
static void host_effects(machine *m, uint32_t addr, uint32_t v)
{
    switch (addr & 0x3F) {
    case 0x10: {
        uint32_t a = CODE + (v & 0xFFE);
        m->mem[a] = 0x4E;
        m->mem[a + 1] = 0x71;
        relevo_invalidate(m->cpu, a, 2);
        break;
    }
    case 0x20:
        relevo_map(m->cpu, HOST_PAGE, RELEVO_PAGE_SIZE, RELEVO_PAGE_HOST);
        break;
    case 0x30:
        relevo_invalidate(m->cpu, CODE, CODE_END - CODE);
        break;
    default:
        break;
    }
}

/* ---- A case ----------------------------------------------------------- */

typedef struct state {
    uint32_t d[8], a[8], pc, usp, isp, msp;
    uint16_t sr;
    relevo_fpreg fp[8];
    uint32_t fpcr, fpsr, fpiar;
    unsigned fpu_pending;
    bool fpu_null;
} state;

static void load_state(machine *m, const state *s)
{
    relevo_reset(m->cpu);   /* out of a double bus fault */
    relevo_regs *r = relevo_get_regs(m->cpu);
    memcpy(r->d, s->d, sizeof r->d);
    memcpy(r->a, s->a, sizeof r->a);
    r->usp = s->usp;
    r->isp = s->isp;
    r->msp = s->msp;
    r->pc = s->pc;
    r->sr = s->sr;
    r->vbr = 0;
    memcpy(r->fp, s->fp, sizeof r->fp);
    r->fpcr = s->fpcr;
    r->fpsr = s->fpsr;
    r->fpiar = s->fpiar;
    relevo_test_fpu_state(m->cpu, NULL, NULL, 1, s->fpu_pending, s->fpu_null);
    m->nlog = 0;
}

static unsigned long cases;

static void dump_code(uint32_t pc, uint32_t end)
{
    printf("  code at %06X:", pc);
    for (uint32_t a = pc; a < end; a += 2)
        printf(" %04X", (unsigned)(init_mem[a] << 8 | init_mem[a + 1]));
    printf("\n");
}

static bool compare(machine *x, machine *y, relevo_status sx,
                    relevo_status sy, bool full_mem)
{
    relevo_regs *a = relevo_get_regs(x->cpu), *b = relevo_get_regs(y->cpu);
    bool ok = true;
#define CMP(field, name)                                                  \
    if (a->field != b->field) {                                           \
        printf("  %s: interp %08X jit %08X\n", name, (unsigned)a->field,  \
               (unsigned)b->field);                                       \
        ok = false;                                                       \
    }
    static const char *const dn[8] = { "D0", "D1", "D2", "D3", "D4", "D5",
                                       "D6", "D7" };
    static const char *const an[8] = { "A0", "A1", "A2", "A3", "A4", "A5",
                                       "A6", "A7" };
    for (int i = 0; i < 8; i++) {
        CMP(d[i], dn[i]);
        CMP(a[i], an[i]);
    }
    CMP(pc, "PC");
    CMP(sr, "SR");
    CMP(usp, "USP");
    CMP(isp, "ISP");
    CMP(msp, "MSP");
    static const char *const fpn[8] = { "FP0", "FP1", "FP2", "FP3", "FP4",
                                        "FP5", "FP6", "FP7" };
    for (int i = 0; i < 8; i++) {
        CMP(fp[i].sign_exponent, fpn[i]);
        if (a->fp[i].mantissa != b->fp[i].mantissa) {
            printf("  %s mantissa: interp %016" PRIX64 " jit %016" PRIX64
                   "\n", fpn[i], a->fp[i].mantissa, b->fp[i].mantissa);
            ok = false;
        }
    }
    CMP(fpcr, "FPCR");
    CMP(fpsr, "FPSR");
    CMP(fpiar, "FPIAR");
#undef CMP
    unsigned xp, yp;
    bool xn, yn;
    relevo_test_fpu_state(x->cpu, &xp, &xn, 0, 0, false);
    relevo_test_fpu_state(y->cpu, &yp, &yn, 0, 0, false);
    if (xp != yp || xn != yn) {
        printf("  FPU pending/null: interp %u/%d jit %u/%d\n", xp, xn, yp,
               yn);
        ok = false;
    }
    if (sx != sy) {
        printf("  status: interp %d jit %d\n", (int)sx, (int)sy);
        ok = false;
    }
    if (relevo_instructions(x->cpu) != relevo_instructions(y->cpu)) {
        printf("  instructions: interp %" PRIu64 " jit %" PRIu64 "\n",
               relevo_instructions(x->cpu), relevo_instructions(y->cpu));
        ok = false;
    }
    if (x->nlog != y->nlog
        || memcmp(x->log, y->log, x->nlog * sizeof x->log[0]) != 0) {
        printf("  host accesses differ (%u / %u)\n", x->nlog, y->nlog);
        ok = false;
    }
    static const uint32_t win[][2] = {
        { 0, 0x8000 }, { CODE, CODE_END }, { WIN - 0x10000, WIN + 0x10000 },
    };
    for (unsigned w = 0; w < 3; w++) {
        uint32_t lo = full_mem ? 0 : win[w][0];
        uint32_t hi = full_mem ? MEM_SIZE : win[w][1];
        if (memcmp(x->mem + lo, y->mem + lo, hi - lo) != 0) {
            for (uint32_t i = lo; i < hi; i++)
                if (x->mem[i] != y->mem[i]) {
                    printf("  memory %06X: interp %02X jit %02X\n", i,
                           x->mem[i], y->mem[i]);
                    break;
                }
            ok = false;
            break;
        }
        if (full_mem)
            break;
    }
    return ok;
}

/* Sets both memories back to init_mem where a case may have changed it. */
static void restore(machine *m, bool full)
{
    if (full) {
        memcpy(m->mem, init_mem, MEM_SIZE);
        relevo_invalidate(m->cpu, 0, MEM_SIZE);
        return;
    }
    memcpy(m->mem, init_mem, 0x8000);
    memcpy(m->mem + CODE, init_mem + CODE, CODE_END - CODE);
    memcpy(m->mem + WIN - 0x10000, init_mem + WIN - 0x10000, 0x20000);
    /* The host wrote the memory: translated code there is stale. */
    relevo_invalidate(m->cpu, 0, 0x8000);
    relevo_invalidate(m->cpu, CODE, CODE_END - CODE);
    relevo_invalidate(m->cpu, WIN - 0x10000, 0x20000);
}

static bool run_case(machine *x, machine *y, unsigned maxlen,
                     bool full_mem)
{
    /* Data and code. */
    for (uint32_t a = WIN - 0x100; a < WIN + WIN_SIZE + 0x100; a += 4)
        put32(init_mem, a, chance(30) ? rnd_addr() : rnd_value());
    unsigned len = 1 + rn(maxlen);
    uint32_t start = CODE + (chance(50) ? 0 : (rn(0x800) & ~1u));
    uint32_t pc = start;
    for (unsigned i = 0; i < len; i++) {
        rnd_insn(pc);
        for (unsigned k = 0; k < nwords && pc + 2 <= CODE_END; k++, pc += 2)
            put16(init_mem, pc, words[k]);
    }
    /* What follows: random words, then NOPs */
    for (uint32_t a = pc; a < pc + 16 && a + 2 <= CODE_END; a += 2)
        put16(init_mem, a, chance(50) ? 0x4E71 : rnd());

    state s;
    for (int i = 0; i < 8; i++) {
        s.d[i] = rnd_value();
        s.a[i] = chance(85) ? rnd_addr() : rnd_value();
    }
    s.sr = (uint16_t)(0x2700 | (rnd() & 0x1F));
    if (chance(20))
        s.sr &= (uint16_t)~0x2000;          /* user mode */
    if (chance(3))
        s.sr |= 0x8000;                     /* trace */
    s.a[7] = WIN + (rn(WIN_SIZE) & ~1u);
    if (chance(5))
        s.a[7] = rnd_addr();
    s.usp = WIN + (rn(WIN_SIZE) & ~1u);
    s.isp = WIN + (rn(WIN_SIZE) & ~1u);
    s.msp = WIN + (rn(WIN_SIZE) & ~1u);
    s.pc = start;
    for (int i = 0; i < 8; i++)
        s.fp[i] = rnd_fp();
    /* rounding and precision; exceptions enabled now and then */
    s.fpcr = rnd() & 0xF0;
    if (chance(15))
        s.fpcr |= rnd() & 0xFF00;
    s.fpsr = rnd() & 0x0FFFFFF8u;
    s.fpiar = rnd();
    s.fpu_pending = chance(4) ? 48 + rn(7) : 0;
    s.fpu_null = chance(10);

    restore(x, full_mem);
    restore(y, full_mem);
    load_state(x, &s);
    load_state(y, &s);
    uint64_t budget = len + (chance(30) ? rn(20) : 0);
    relevo_status sx = relevo_run(x->cpu, budget);
    relevo_status sy = relevo_run(y->cpu, budget);
    cases++;
    if (compare(x, y, sx, sy, full_mem))
        return true;
    printf("case %lu differs (budget %" PRIu64 ", SR %04X):\n", cases,
           budget, s.sr);
    dump_code(start, pc + 2 > CODE_END ? CODE_END : pc);
    for (int i = 0; i < 8; i++)
        printf("  D%d=%08X A%d=%08X\n", i, s.d[i], i, s.a[i]);
    return false;
}

int main(int argc, char **argv)
{
    unsigned long n = 20000;
    unsigned maxlen = 8;
    bool no_movbe = false, eager = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            long m = strtol(argv[++i], NULL, 10);
            model = m == 68010 ? RELEVO_68010 : m == 68020 ? RELEVO_68020
                  : m == 68030 ? RELEVO_68030 : m == 68040 ? RELEVO_68040
                  : RELEVO_68000;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            maxlen = (unsigned)strtoul(argv[++i], NULL, 10);
            if (maxlen == 0)
                maxlen = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            rng_state = strtoull(argv[++i], NULL, 0) | 1;
        } else if (strcmp(argv[i], "-x") == 0) {
            no_movbe = true;
        } else if (strcmp(argv[i], "-e") == 0) {
            eager = true;
        } else {
            fprintf(stderr, "usage: jitdiff [-m MODEL] [-n CASES] [-k MAXLEN]"
                            " [-s SEED] [-x] [-e]\n");
            return 2;
        }
    }
    gen_fpu = model_fpu();
    setup_static();
    machine x, y;
    if (!machine_init(&x, RELEVO_ENGINE_INTERP)) {
        fprintf(stderr, "jitdiff: cannot create the CPUs\n");
        return 2;
    }
    if (!machine_init(&y, RELEVO_ENGINE_JIT)) {
        printf("jitdiff: no JIT on this machine: skipped\n");
        return 0;
    }
    if (no_movbe)
        jit_set_movbe(y.cpu, false);
    if (eager)
        jit_set_lazy(y.cpu, false);
    restore(&x, true);
    restore(&y, true);
    for (unsigned long i = 0; i < n; i++)
        if (!run_case(&x, &y, maxlen, (i & 255) == 255))
            return 1;
    jit_stats js;
    jit_get_stats(y.cpu, &js);
    printf("jitdiff 680%02d%s%s: %lu cases of up to %u instructions, all equal;"
           " %" PRIu64 " blocks (%" PRIu64 " instructions) translated, %"
           PRIu64 " run, %" PRIu64 " left for the interpreter, %" PRIu64
           " instructions interpreted, %" PRIu64 " for host pages\n",
           model == RELEVO_68000 ? 0 : model == RELEVO_68010 ? 10
           : model == RELEVO_68020 ? 20 : model == RELEVO_68030 ? 30 : 40,
           no_movbe ? " (BSWAP)" : "", eager ? " (eager flags)" : "", cases, maxlen, js.blocks, js.insns,
           js.runs, js.bails, js.interp, js.host);
    relevo_destroy(x.cpu);
    relevo_destroy(y.cpu);
    free(x.mem);
    free(y.mem);
    return 0;
}
