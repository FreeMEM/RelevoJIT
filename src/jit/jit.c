/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT engine's dispatcher and its cache of translations.
 *
 * Blocks are found by the 68k address of their first instruction. Each
 * keeps a hash of the 68k code it was made from, checked every time the
 * block is entered (except for blocks entirely in ROM pages): code that
 * changed gets translated again, without the program having to say so (the
 * idea of Emu68's checksummed translation units). relevo_invalidate() and
 * relevo_map() drop translations at once.
 *
 * A block runs from its first instruction up to a jump or branch, an
 * instruction the JIT does not translate, JIT_MAX_INSNS instructions, the
 * budget, or a 68k register more than the x86 registers it can hold.
 * Between blocks the dispatcher does what the interpreter does between
 * instructions: interrupts, trace, STOP, the stop request, the budget and
 * the exit at PC 0. Untranslated instructions, and those a block left for
 * the interpreter (see jit.h), run in the interpreter one at a time.
 */
#include <stdlib.h>
#include <string.h>

#include "jit_internal.h"

static unsigned popcount16(uint16_t v)
{
    unsigned n = 0;
    for (; v; v &= (uint16_t)(v - 1))
        n++;
    return n;
}

static uint64_t code_hash(const uint8_t *p, uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15u ^ len;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDu;
        h ^= h >> 32;
        p += 8;
        len -= 8;
    }
    uint64_t v = 0;
    memcpy(&v, p, len);
    h = (h ^ v) * 0xC4CEB9FE1A85EC53u;
    return h ^ (h >> 29);
}

static unsigned bucket_of(uint32_t pc)
{
    return (pc >> 1 ^ pc >> 13) & (JIT_HASH_SIZE - 1);
}

bool jit_available(void)
{
    return jit_backend_available();
}

bool jit_create(relevo_cpu *c)
{
    if (!jit_backend_available())
        return false;
    jit_state *j = calloc(1, sizeof *j);
    if (j == NULL)
        return false;
    j->pool = calloc(JIT_POOL_SIZE, sizeof *j->pool);
    if (j->pool == NULL || !codemem_create(&j->mem, JIT_CODE_SIZE)) {
        free(j->pool);
        free(j);
        return false;
    }
    j->has_movbe = jit_backend_has_movbe();
    j->movbe = j->has_movbe;
    bool ok = codemem_begin_write(&j->mem);
    ok = ok && jit_backend_init(c, j);
    codemem_end_write(&j->mem);
    if (!ok) {
        codemem_destroy(&j->mem);
        free(j->pool);
        free(j);
        return false;
    }
    c->jit = j;
    return true;
}

void jit_destroy(relevo_cpu *c)
{
    jit_state *j = c->jit;
    if (j == NULL)
        return;
    codemem_destroy(&j->mem);
    free(j->pool);
    free(j);
    c->jit = NULL;
}

void jit_flush(relevo_cpu *c)
{
    jit_state *j = c->jit;
    if (j == NULL)
        return;
    memset(j->hash, 0, sizeof j->hash);
    j->pool_used = 0;
    j->free_list = NULL;
    j->used = j->base_used;
    j->stats.flushes++;
}

static void drop(jit_state *j, jit_block *b)
{
    jit_block **p = &j->hash[bucket_of(b->pc)];
    while (*p != b)
        p = &(*p)->next;
    *p = b->next;
    b->next = j->free_list;
    j->free_list = b;
}

void jit_invalidate(relevo_cpu *c, uint32_t address, uint32_t size)
{
    jit_state *j = c->jit;
    if (j == NULL || size == 0)
        return;
    uint64_t lo = address & c->addr_mask, hi = lo + size;
    for (unsigned k = 0; k < JIT_HASH_SIZE; k++) {
        jit_block **p = &j->hash[k];
        while (*p != NULL) {
            jit_block *b = *p;
            uint64_t s = b->start, e = s + (b->len ? b->len : 1);
            if (s < hi && lo < e) {
                *p = b->next;
                b->next = j->free_list;
                j->free_list = b;
            } else {
                p = &b->next;
            }
        }
    }
}

static jit_block *lookup(jit_state *j, uint32_t pc)
{
    for (jit_block *b = j->hash[bucket_of(pc)]; b != NULL; b = b->next)
        if (b->pc == pc)
            return b;
    return NULL;
}

static bool block_valid(const relevo_cpu *c, const jit_block *b)
{
    return b->rom || b->len == 0
        || code_hash(c->mem + b->start, b->len) == b->hash;
}

static jit_block *new_block(jit_state *j)
{
    jit_block *b = j->free_list;
    if (b != NULL) {
        j->free_list = b->next;
    } else {
        if (j->pool_used >= JIT_POOL_SIZE)
            return NULL;
        b = &j->pool[j->pool_used++];
    }
    memset(b, 0, sizeof *b);
    return b;
}

/* Decodes the block at `pc` into j->insns; returns how many instructions
 * it has. */
static unsigned form_block(relevo_cpu *c, jit_state *j, uint32_t pc,
                           unsigned max, bool *rom)
{
    uint32_t start = pc & c->addr_mask;
    uint32_t at = pc;
    uint16_t regs = 0;
    unsigned n = 0;
    *rom = true;
    while (n < max) {
        jit_insn *in = &j->insns[n];
        bool r = *rom;
        if (!jit_decode(c, at, in, &r))
            break;
        if ((uint64_t)(at & c->addr_mask) != (uint64_t)start + (at - pc))
            break;
        if (popcount16((uint16_t)(regs | in->regs)) > JIT_HOST_REGS)
            break;
        regs |= in->regs;
        *rom = r;
        n++;
        at += in->len;
        if (in->ends_block)
            break;
    }
    return n;
}

/* Translates the block at the PC with at most `max` instructions and
 * puts it in the cache. Returns NULL only when out of descriptors or
 * code memory (the caller flushes and tries again). */
static jit_block *translate(relevo_cpu *c, jit_state *j, unsigned max)
{
    uint32_t pc = c->r.pc;
    bool rom;
    unsigned n = form_block(c, j, pc, max, &rom);
    jit_block *b = new_block(j);
    if (b == NULL)
        return NULL;
    b->pc = pc;
    b->start = pc & c->addr_mask;
    if (n == 0) {
        /* Not translated: remember that, for the bytes that decided it. */
        uint8_t kind = c->pages[b->start >> RELEVO_PAGE_SHIFT];
        uint32_t room = RELEVO_PAGE_SIZE - (b->start & (RELEVO_PAGE_SIZE - 1));
        b->len = page_direct_read(kind) ? (room < 8 ? room : 8) : 0;
        b->rom = kind == RELEVO_PAGE_ROM;
    } else {
        const jit_insn *last = &j->insns[n - 1];
        b->len = last->pc + last->len - pc;
        b->ninsns = (uint16_t)n;
        b->cut = n == max && !last->ends_block && max < JIT_MAX_INSNS;
        b->rom = rom;
        if (!codemem_begin_write(&j->mem)) {
            b->next = j->free_list;
            j->free_list = b;
            return NULL;
        }
        b->code = jit_backend_translate(c, j, n);
        codemem_end_write(&j->mem);
        if (b->code == NULL) {
            b->next = j->free_list;
            j->free_list = b;
            return NULL;
        }
        j->stats.blocks++;
        j->stats.insns += n;
    }
    b->hash = b->len ? code_hash(c->mem + b->start, b->len) : 0;
    unsigned k = bucket_of(pc);
    b->next = j->hash[k];
    j->hash[k] = b;
    return b;
}

/* The block to run at the PC, or NULL for the interpreter. */
static jit_block *get_block(relevo_cpu *c, jit_state *j)
{
    jit_block *b = lookup(j, c->r.pc);
    if (b != NULL && !block_valid(c, b)) {
        drop(j, b);
        b = NULL;
    }
    if (b != NULL && b->cut && c->budget > b->ninsns) {
        drop(j, b);
        b = NULL;
    }
    if (b == NULL) {
        unsigned max = c->budget < JIT_MAX_INSNS ? (unsigned)c->budget
                                                 : JIT_MAX_INSNS;
        b = translate(c, j, max);
        if (b == NULL) {
            jit_flush(c);
            b = translate(c, j, max);
        }
    }
    if (b == NULL || b->code == NULL || b->ninsns > c->budget)
        return NULL;
    return b;
}

relevo_status jit_run_loop(relevo_cpu *c)
{
    jit_state *j = c->jit;
    while (c->budget != 0) {
        atomic_store_explicit(&c->attention, 0, memory_order_relaxed);
        if (UNLIKELY(atomic_load_explicit(&c->stop_request,
                                          memory_order_relaxed))) {
            atomic_store_explicit(&c->stop_request, false,
                                  memory_order_relaxed);
            return RELEVO_HOST_STOP;
        }
        if (UNLIKELY(cpu_between_needed(c)) && !cpu_between_insns(c))
            return RELEVO_STOPPED;

        if (LIKELY(!j->force_interp && !(c->r.sr & RELEVO_SR_T1))) {
            jit_block *b = get_block(c, j);
            if (b != NULL) {
                j->stats.runs++;
                if (jit_backend_enter(c, j, b->code) != 0) {
                    j->stats.bails++;
                    j->force_interp = true;
                    continue;
                }
                if (UNLIKELY(c->r.pc == 0
                             && (c->options & RELEVO_OPT_EXIT_ON_PC_ZERO)))
                    return RELEVO_EXIT_PC_ZERO;
                continue;
            }
        }
        j->force_interp = false;
        j->stats.interp++;
        cpu_interp_step(c);
        if (UNLIKELY(c->exiting))
            return c->exit;
        if (UNLIKELY(c->r.pc == 0 && (c->options & RELEVO_OPT_EXIT_ON_PC_ZERO)))
            return RELEVO_EXIT_PC_ZERO;
    }
    return RELEVO_BUDGET;
}

void jit_get_stats(const relevo_cpu *c, jit_stats *out)
{
    if (c->jit != NULL)
        *out = c->jit->stats;
    else
        memset(out, 0, sizeof *out);
}

void jit_set_movbe(relevo_cpu *c, bool on)
{
    if (c->jit == NULL)
        return;
    c->jit->movbe = on && c->jit->has_movbe;
    jit_flush(c);
}

bool jit_translatable(relevo_cpu *c, uint32_t pc)
{
    if (c->jit == NULL)
        return false;
    jit_insn in;
    bool rom = true;
    return jit_decode(c, pc, &in, &rom);
}
