/* SPDX-License-Identifier: MPL-2.0
 *
 * The bench against the oracle, RelevoJIT's side (see tools/oraculo/oraculo.py).
 *
 *   oraculo [-v N] [-o DIR] tests/oraculo/NAME.MODEL.txt...
 *   oraculo -d MODEL NAME.bin
 *
 * For each file, runs tests/oraculo/NAME.bin at $140000 on a MODEL
 * (68020, 68040...) CPU in supervisor mode, formats its records as the
 * oracle did, and compares line by line ('?' in the oracle matches any
 * digit; a line "NNNN x" is a case not compared). Prints passed/total per file and the failing cases per
 * instruction; -v N shows the first N failing lines in full. -o DIR also
 * writes RelevoJIT's lines to DIR/NAME.MODEL.txt. Exits with 1 if any case
 * differs. -d prints every record of a program, without comparing (what
 * tools/oraculo/oraculo.py relevo uses).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relevo.h"

#define MEM_SIZE    0x200000u
#define CORE        0x140000u
#define CORE_MAX    0x1C000u
#define RESPTR      0x15C008u
#define DONE        0x15C03Cu
#define RESULTS     0x160000u
#define REC_SIZE    132u
#define RET_SP      0x17FF00u

static uint8_t mem[MEM_SIZE];

static uint32_t get32(uint32_t a)
{
    return (uint32_t)mem[a] << 24 | (uint32_t)mem[a + 1] << 16
         | (uint32_t)mem[a + 2] << 8 | mem[a + 3];
}

static uint32_t get16(uint32_t a)
{
    return (uint32_t)mem[a] << 8 | mem[a + 1];
}

/* The fields part of a line, as tools/oraculo/oraculo.py writes it. */
static void format_record(char *out, size_t size, unsigned index,
                          uint32_t rec)
{
    size_t n = (size_t)snprintf(out, size, "%04u d", index);
    for (unsigned i = 0; i < 8; i++)
        n += (size_t)snprintf(out + n, size - n, " %08x", get32(rec + 4 * i));
    n += (size_t)snprintf(out + n, size - n, " a");
    for (unsigned i = 0; i < 8; i++)
        n += (size_t)snprintf(out + n, size - n, " %08x",
                              get32(rec + 32 + 4 * i));
    n += (size_t)snprintf(out + n, size - n, " sr %04x v %04x b ",
                          get16(rec + 64), get16(rec + 66));
    for (unsigned i = 0; i < 32; i++)
        n += (size_t)snprintf(out + n, size - n, "%02x", mem[rec + 68 + i]);
    n += (size_t)snprintf(out + n, size - n, " f ");
    if (get16(rec + 66) == 0xFFFF) {    /* no exception: FRAME is 0 */
        snprintf(out + n, size - n, "-");
        return;
    }
    for (unsigned i = 0; i < 32; i++)
        n += (size_t)snprintf(out + n, size - n, "%02x", mem[rec + 100 + i]);
}

static bool fields_match(const char *expected, const char *got)
{
    size_t i = 0;
    for (; expected[i] && got[i]; i++)
        if (expected[i] != got[i] && expected[i] != '?')
            return false;
    return expected[i] == 0 && got[i] == 0;
}

/* Field names, for the report: the tokens after the index. */
static const char *const field_names[] = {
    "d", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "sr", "SR", "v", "vector", "b", "BUF", "f", "FRAME"
};

/* The next space-separated token of *s, copied to tok. */
static bool next_token(const char **s, char *tok, size_t size)
{
    while (**s == ' ')
        (*s)++;
    if (**s == 0)
        return false;
    size_t n = 0;
    while (**s && **s != ' ') {
        if (n + 1 < size)
            tok[n++] = **s;
        (*s)++;
    }
    tok[n] = 0;
    return true;
}

static void report_fields(const char *expected, const char *got)
{
    char te[80], tg[80];
    unsigned k = 0;
    printf("        ");
    while (next_token(&expected, te, sizeof te)
           && next_token(&got, tg, sizeof tg)) {
        if (k > 0 && !fields_match(te, tg))
            printf(" %s: %s / %s;",
                   k - 1 < sizeof field_names / sizeof *field_names
                       ? field_names[k - 1] : "?", te, tg);
        k++;
    }
    printf("  (oracle / RelevoJIT)\n");
}

static bool parse_model(const char *s, relevo_model *m)
{
    static const struct { const char *name; relevo_model model; } models[] = {
        { "68000", RELEVO_68000 }, { "68010", RELEVO_68010 },
        { "68020", RELEVO_68020 }, { "68030", RELEVO_68030 },
        { "68040", RELEVO_68040 }, { "68060", RELEVO_68060 },
    };
    for (size_t i = 0; i < sizeof models / sizeof *models; i++)
        if (strcmp(s, models[i].name) == 0) {
            *m = models[i].model;
            return true;
        }
    return false;
}

/* Runs the program; returns the number of records, or -1. */
static long run_program(const char *bin, relevo_model model)
{
    FILE *f = fopen(bin, "rb");
    if (f == NULL) {
        perror(bin);
        return -1;
    }
    memset(mem, 0, sizeof mem);
    size_t n = fread(mem + CORE, 1, CORE_MAX, f);
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "%s: empty\n", bin);
        return -1;
    }

    relevo_config cfg = {0};
    cfg.model = model;
    /* The FPU of the oracle: a 68882 next to the 68020 and 68030 (the
     * programs without FPU instructions do not see it), the 68040's own. */
    if (model == RELEVO_68020 || model == RELEVO_68030)
        cfg.fpu = RELEVO_FPU_68882;
    else if (model >= RELEVO_68040)
        cfg.fpu = RELEVO_FPU_INTERNAL;
    cfg.memory = mem;
    cfg.memory_size = MEM_SIZE;
    cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
    relevo_cpu *c = relevo_create(&cfg);
    if (c == NULL) {
        fprintf(stderr, "cannot create the CPU\n");
        return -1;
    }
    relevo_regs *r = relevo_get_regs(c);
    relevo_set_sr(c, 0x2700);
    r->a[7] = RET_SP;           /* the return address there is 0 */
    r->pc = CORE;
    relevo_status st = relevo_run(c, 50000000);
    uint32_t pc = r->pc;
    relevo_destroy(c);
    if (st != RELEVO_EXIT_PC_ZERO || get32(DONE) != 0x444F4E45)
        fprintf(stderr, "%s: the program did not finish (status %d, PC "
                "%08x)\n", bin, (int)st, pc);
    uint32_t end = get32(RESPTR);
    if (end < RESULTS || end > RESULTS + 0x10000)
        return -1;
    return (long)((end - RESULTS) / REC_SIZE);
}

typedef struct insn_count {
    char name[24];
    unsigned failed, total;
} insn_count;

static int check_file(const char *path, long verbose, const char *outdir)
{
    /* NAME.MODEL.txt -> NAME.bin, MODEL */
    char bin[1024], model_s[16];
    const char *dot2 = strrchr(path, '.');
    const char *dot1 = NULL;
    for (const char *p = path; p < dot2; p++)
        if (*p == '.')
            dot1 = p;
    relevo_model model;
    if (dot2 == NULL || dot1 == NULL || dot2 - dot1 - 1 >= (long)sizeof model_s) {
        fprintf(stderr, "%s: not NAME.MODEL.txt\n", path);
        return 1;
    }
    memcpy(model_s, dot1 + 1, (size_t)(dot2 - dot1 - 1));
    model_s[dot2 - dot1 - 1] = 0;
    if (!parse_model(model_s, &model)) {
        fprintf(stderr, "%s: unknown model %s\n", path, model_s);
        return 1;
    }
    snprintf(bin, sizeof bin, "%.*s.bin", (int)(dot1 - path), path);

    long records = run_program(bin, model);
    if (records < 0) {
        printf("%-34s no results\n", path);
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    FILE *out = NULL;
    if (outdir != NULL) {
        char name[1024];
        const char *base = strrchr(path, '/');
        snprintf(name, sizeof name, "%s/%s", outdir, base ? base + 1 : path);
        out = fopen(name, "w");
    }

    static insn_count counts[256];
    unsigned ncounts = 0, passed = 0, total = 0, skips = 0;
    long shown = 0;
    char line[1024], got[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        line[strcspn(line, "\n")] = 0;
        char *sep = strstr(line, " ; ");
        const char *desc = sep ? sep + 3 : "";
        if (sep)
            *sep = 0;
        unsigned index = (unsigned)strtoul(line, NULL, 10);
        total++;
        if ((long)index >= records) {
            snprintf(got, sizeof got, "(missing)");
        } else {
            format_record(got, sizeof got, index,
                          RESULTS + index * REC_SIZE);
        }
        if (out)
            fprintf(out, "%s ; %s\n", got, desc);
        /* "NNNN x": a case the oracle does not decide (see oraculo.py) */
        size_t len = strlen(line);
        bool skipped = len >= 2 && strcmp(line + len - 2, " x") == 0;
        bool ok = skipped || fields_match(line, got);
        skips += skipped;
        passed += ok;

        char insn[24];
        snprintf(insn, sizeof insn, "%s", desc);
        insn[strcspn(insn, " ")] = 0;
        unsigned k = 0;
        while (k < ncounts && strcmp(counts[k].name, insn) != 0)
            k++;
        if (k == ncounts && ncounts < 256) {
            snprintf(counts[k].name, sizeof counts[k].name, "%s", insn);
            counts[k].failed = counts[k].total = 0;
            ncounts++;
        }
        if (k < ncounts) {
            counts[k].total++;
            counts[k].failed += !ok;
        }
        if (!ok && shown < verbose) {
            printf("    %.4s %s\n", line, desc);
            report_fields(line, got);
            shown++;
        }
    }
    fclose(f);
    if (out)
        fclose(out);
    printf("%-34s %4u/%-4u%s", path, passed, total,
           passed == total ? "" : "  FAIL");
    if (skips)
        printf("  (%u not compared)", skips);
    printf("\n");
    for (unsigned k = 0; k < ncounts; k++)
        if (counts[k].failed)
            printf("    %-12s %u of %u differ\n", counts[k].name,
                   counts[k].failed, counts[k].total);
    return passed == total ? 0 : 1;
}

int main(int argc, char **argv)
{
    long verbose = 0;
    const char *outdir = NULL;
    int i = 1;
    while (i + 1 < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-v") == 0)
            verbose = strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "-o") == 0)
            outdir = argv[i + 1];
        else
            break;
        i += 2;
    }
    if (argc == 4 && strcmp(argv[1], "-d") == 0) {
        relevo_model model;
        if (!parse_model(argv[2], &model)) {
            fprintf(stderr, "unknown model %s\n", argv[2]);
            return 2;
        }
        long records = run_program(argv[3], model);
        char got[512];
        for (long k = 0; k < records; k++) {
            format_record(got, sizeof got, (unsigned)k,
                          RESULTS + (uint32_t)k * REC_SIZE);
            puts(got);
        }
        return records < 0;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: oraculo [-v N] [-o DIR] NAME.MODEL.txt...\n");
        return 2;
    }
    int status = 0;
    for (; i < argc; i++)
        status |= check_file(argv[i], verbose, outdir);
    return status;
}
