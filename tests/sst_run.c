/* SPDX-License-Identifier: MPL-2.0 */
#include "sst_run.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE 0x1000000u
#define MASK (MEM_SIZE - 1)

bool sst_env_init(sst_env *env)
{
    return sst_env_init_engine(env, RELEVO_ENGINE_INTERP);
}

bool sst_env_init_engine(sst_env *env, relevo_engine engine)
{
    env->mem = calloc(1, MEM_SIZE);
    if (env->mem == NULL)
        return false;
    relevo_config cfg = {0};
    cfg.model = RELEVO_68000;
    cfg.engine = engine;
    cfg.memory = env->mem;
    cfg.memory_size = MEM_SIZE;
    env->cpu = relevo_create(&cfg);
    if (env->cpu == NULL) {
        free(env->mem);
        return false;
    }
    return true;
}

void sst_env_free(sst_env *env)
{
    relevo_destroy(env->cpu);
    free(env->mem);
}

static void clear_state_memory(sst_env *env, const sst_state *s)
{
    for (size_t i = 0; i < s->ram_count; i++)
        env->mem[s->ram[i].address & MASK] = 0;
    for (uint32_t i = 0; i < 4; i++)
        env->mem[(s->reg[SST_PC] - 4 + i) & MASK] = 0;
}

/* Clears what the CPU may have pushed between the initial and final stack
 * pointers, even where the test does not expect it (a wrong or missing
 * instruction takes an exception), so that it cannot leak into the next
 * test. */
static void clear_stack(sst_env *env, uint32_t from, uint32_t to)
{
    uint32_t lo = from < to ? from : to, hi = from < to ? to : from;
    if (hi - lo > 64)
        return;
    for (uint32_t a = lo - 4; a != hi + 4; a++)
        env->mem[a & MASK] = 0;
}

void sst_env_clear(sst_env *env)
{
    memset(env->mem, 0, MEM_SIZE);
}

typedef struct why_buf {
    char *buf;
    size_t size, len;
    unsigned count;
} why_buf;

static void why_add(why_buf *w, const char *fmt, ...)
{
    w->count++;
    if (w->count > 4 || w->len + 1 >= w->size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->size - w->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        w->len += (size_t)n < w->size - w->len ? (size_t)n
                                                : w->size - w->len - 1;
}

bool sst_run_one(sst_env *env, const sst_test *t, char *why, size_t why_size)
{
    const sst_state *in = &t->initial, *out = &t->final;
    relevo_cpu *cpu = env->cpu;

    for (size_t i = 0; i < in->ram_count; i++)
        env->mem[in->ram[i].address & MASK] = in->ram[i].value;
    /* The two prefetched words: the opcode and the word after it. */
    uint32_t start = in->reg[SST_PC] - 4;
    for (int i = 0; i < 2; i++) {
        env->mem[(start + 2 * i) & MASK] = (uint8_t)(in->prefetch[i] >> 8);
        env->mem[(start + 2 * i + 1) & MASK] = (uint8_t)in->prefetch[i];
    }

    relevo_reset(cpu);
    relevo_regs *r = relevo_get_regs(cpu);
    for (int i = 0; i < 8; i++)
        r->d[i] = in->reg[SST_D0 + i];
    for (int i = 0; i < 7; i++)
        r->a[i] = in->reg[SST_A0 + i];
    r->sr = (uint16_t)in->reg[SST_SR];
    r->usp = in->reg[SST_USP];
    r->isp = in->reg[SST_SSP];
    r->a[7] = (r->sr & RELEVO_SR_S) ? r->isp : r->usp;
    r->pc = start;

    uint64_t cycles = relevo_cycles(cpu);
    relevo_status st = relevo_run(cpu, 1);
    env->cycles = (uint32_t)(relevo_cycles(cpu) - cycles);

    uint32_t got[SST_NREGS];
    for (int i = 0; i < 8; i++)
        got[SST_D0 + i] = r->d[i];
    for (int i = 0; i < 7; i++)
        got[SST_A0 + i] = r->a[i];
    bool super = r->sr & RELEVO_SR_S;
    got[SST_USP] = super ? r->usp : r->a[7];
    got[SST_SSP] = super ? r->a[7] : r->isp;
    got[SST_SR] = r->sr;
    /* The tests give the next prefetch address; a stopped CPU has not
     * prefetched past STOP. */
    got[SST_PC] = st == RELEVO_STOPPED ? r->pc : r->pc + 4;

    why_buf w = { why, why_size, 0, 0 };
    if (why_size > 0)
        why[0] = 0;
    for (int i = 0; i < SST_NREGS; i++)
        if (got[i] != out->reg[i])
            why_add(&w, "%s%s=%08X (want %08X)", w.len ? ", " : "",
                    sst_reg_names[i], got[i], out->reg[i]);
    for (size_t i = 0; i < out->ram_count; i++) {
        uint32_t a = out->ram[i].address & MASK;
        if (env->mem[a] != out->ram[i].value)
            why_add(&w, "%s[%06X]=%02X (want %02X)", w.len ? ", " : "", a,
                    env->mem[a], out->ram[i].value);
    }
    if (w.count > 4)
        why_add(&w, ", ...");

    clear_state_memory(env, in);
    clear_state_memory(env, out);
    clear_stack(env, in->reg[SST_SSP], got[SST_SSP]);
    clear_stack(env, in->reg[SST_USP], got[SST_USP]);
    return w.count == 0;
}
