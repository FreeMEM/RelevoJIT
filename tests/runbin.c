/* SPDX-License-Identifier: MPL-2.0
 *
 * runbin: runs flat 68k binaries (such as DashOS's jit-host/pruebas68k) on
 * the interpreter and on the JIT, and prints one line per binary and
 * engine:
 *
 *   <name> <engine>: <end> insns=<n> regs=<hash> mem=<hash>
 *
 * Every binary gets the same machine: 2 MB of RAM from address 0, the
 * binary at $140000 (at most $30000 bytes), the stack at $1FEFFC with a
 * return address of 0 on it, everything else zeroed, and a reset (the ISP
 * and PC from addresses 0 and 4). It runs until the PC reaches 0, or
 * another end (at most 100 million instructions). Each engine has one CPU
 * for all the binaries, so the JIT must drop the code of the previous one:
 * the binary is written straight into the memory block, followed by
 * relevo_invalidate().
 *
 * `regs` is FNV-1a over D0-D7, A0-A7, PC, SR, FP0-FP7 (sign and exponent,
 * then mantissa), FPCR, FPSR and FPIAR, each big-endian; `mem` is FNV-1a
 * over $100000-$1FFFFF, the same sum as DashOS's jit-host prints. DashOS
 * prints the same lines from its own runner (usuario/src/bin/relevo.rs in
 * the x86 branch), to compare.
 *
 *   runbin [-m 68000|68010|68020|68030|68040|68060] [-v] file.bin...
 *
 * The default is a 68040 with its FPU, what the pruebas68k programs are
 * built for. -v also prints the registers.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relevo.h"

#define MEM_SIZE    0x200000u
#define LOAD_AT     0x140000u
#define LOAD_MAX    0x30000u
#define STACK       0x1FEFFCu
#define MAX_INSNS   100000000u

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t fnv_be(uint64_t h, uint64_t v, unsigned bytes)
{
    uint8_t b[8];
    for (unsigned i = 0; i < bytes; i++)
        b[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    return fnv(h, b, bytes);
}

static uint64_t regs_hash(const relevo_regs *r)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 8; i++)
        h = fnv_be(h, r->d[i], 4);
    for (int i = 0; i < 8; i++)
        h = fnv_be(h, r->a[i], 4);
    h = fnv_be(h, r->pc, 4);
    h = fnv_be(h, r->sr, 2);
    for (int i = 0; i < 8; i++) {
        h = fnv_be(h, r->fp[i].sign_exponent, 2);
        h = fnv_be(h, r->fp[i].mantissa, 8);
    }
    h = fnv_be(h, r->fpcr, 4);
    h = fnv_be(h, r->fpsr, 4);
    return fnv_be(h, r->fpiar, 4);
}

static const char *end_name(relevo_status st)
{
    switch (st) {
    case RELEVO_EXIT_PC_ZERO: return "pc0";
    case RELEVO_STOPPED:      return "stop";
    case RELEVO_HOST_STOP:    return "host";
    case RELEVO_HALTED:       return "halted";
    default:                  return "budget";
    }
}

typedef struct engine {
    const char *name;
    uint8_t *mem;
    relevo_cpu *cpu;
} engine;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Runs one binary; returns 0 if it ended at PC 0. */
static int run(engine *e, const char *name, const uint8_t *bin, size_t n,
               int verbose)
{
    memset(e->mem, 0, MEM_SIZE);
    put32(e->mem, STACK);
    put32(e->mem + 4, LOAD_AT);
    memcpy(e->mem + LOAD_AT, bin, n);
    relevo_invalidate(e->cpu, 0, MEM_SIZE);

    relevo_regs *r = relevo_get_regs(e->cpu);
    memset(r->d, 0, sizeof r->d);
    memset(r->a, 0, sizeof r->a);
    r->usp = r->msp = 0;
    relevo_reset(e->cpu);

    uint64_t start = relevo_instructions(e->cpu);
    relevo_status st;
    do
        st = relevo_run(e->cpu, 1000000);
    while (st == RELEVO_BUDGET
           && relevo_instructions(e->cpu) - start < MAX_INSNS);

    printf("%s %s: %s insns=%" PRIu64 " regs=%016" PRIx64 " mem=%016" PRIx64
           "\n", name, e->name, end_name(st),
           relevo_instructions(e->cpu) - start, regs_hash(r),
           fnv(0xcbf29ce484222325ull, e->mem + 0x100000, 0x100000));
    if (verbose) {
        printf(" ");
        for (int i = 0; i < 8; i++)
            printf(" D%d=%08" PRIx32, i, r->d[i]);
        printf("\n ");
        for (int i = 0; i < 8; i++)
            printf(" A%d=%08" PRIx32, i, r->a[i]);
        printf("\n  PC=%08" PRIx32 " SR=%04x FPCR=%08" PRIx32 " FPSR=%08"
               PRIx32 " FPIAR=%08" PRIx32 "\n ", r->pc, r->sr, r->fpcr,
               r->fpsr, r->fpiar);
        for (int i = 0; i < 8; i++)
            printf(" FP%d=%04x:%016" PRIx64 "%s", i, r->fp[i].sign_exponent,
                   r->fp[i].mantissa, i == 3 ? "\n " : "");
        printf("\n");
    }
    return st == RELEVO_EXIT_PC_ZERO ? 0 : 1;
}

static const char *base_name(const char *path, char *out, size_t size)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t n = strlen(b);
    if (n > 4 && strcmp(b + n - 4, ".bin") == 0)
        n -= 4;
    if (n >= size)
        n = size - 1;
    memcpy(out, b, n);
    out[n] = 0;
    return out;
}

int main(int argc, char **argv)
{
    relevo_model model = RELEVO_68040;
    int verbose = 0, first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[first], "-m") == 0 && first + 1 < argc) {
            static const char *names[] = { "68000", "68010", "68020",
                                           "68030", "68040", "68060" };
            const char *m = argv[++first];
            unsigned i = 0;
            while (i < 6 && strcmp(names[i], m) != 0)
                i++;
            if (i == 6) {
                fprintf(stderr, "runbin: unknown model %s\n", m);
                return 2;
            }
            model = (relevo_model)i;
        } else {
            fprintf(stderr, "usage: runbin [-m model] [-v] file.bin...\n");
            return 2;
        }
    }

    engine eng[2] = { { "interp", NULL, NULL }, { "jit", NULL, NULL } };
    for (int k = 0; k < 2; k++) {
        relevo_config cfg = { 0 };
        cfg.model = model;
        cfg.fpu = model >= RELEVO_68040 ? RELEVO_FPU_INTERNAL
                : model >= RELEVO_68020 ? RELEVO_FPU_68882 : RELEVO_FPU_NONE;
        cfg.engine = k == 0 ? RELEVO_ENGINE_INTERP : RELEVO_ENGINE_JIT;
        cfg.memory = eng[k].mem = calloc(1, MEM_SIZE);
        cfg.memory_size = MEM_SIZE;
        cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
        eng[k].cpu = eng[k].mem ? relevo_create(&cfg) : NULL;
        if (eng[k].cpu == NULL) {
            fprintf(stderr, "runbin: no %s engine here\n", eng[k].name);
            return 1;
        }
    }

    static uint8_t bin[LOAD_MAX];
    int failed = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n = f ? fread(bin, 1, sizeof bin, f) : 0;
        if (f)
            fclose(f);
        if (n == 0) {
            fprintf(stderr, "runbin: cannot read %s\n", argv[i]);
            failed = 1;
            continue;
        }
        char name[64];
        base_name(argv[i], name, sizeof name);
        for (int k = 0; k < 2; k++)
            failed |= run(&eng[k], name, bin, n, verbose);
    }
    for (int k = 0; k < 2; k++) {
        relevo_destroy(eng[k].cpu);
        free(eng[k].mem);
    }
    return failed;
}
