/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT engine's dispatcher and its cache of translations.
 *
 * Blocks are found by the 68k address of their first instruction. A block
 * runs from its first instruction up to a jump or branch, an instruction
 * the JIT does not translate, JIT_MAX_INSNS instructions, the budget, or a
 * 68k register more than the x86 registers it can hold.
 *
 * Chaining: a block that leaves for a PC known when it was translated
 * (a branch, the next instruction) does it through a jump that first goes
 * back to the dispatcher; the dispatcher then patches it to go straight to
 * the block at that PC, and records the link in that block so that
 * dropping it puts the jump back. Indirect jumps (RTS, JMP (An)) look the
 * PC up in a small cache of targets (jit_state.ind) from translated code.
 * Every block checks the budget and the attention flag when it is entered,
 * so a chain of blocks stops where the dispatcher would.
 *
 * Between blocks the dispatcher does what the interpreter does between
 * instructions: interrupts, trace, STOP, the stop request, the budget and
 * the exit at PC 0. Untranslated instructions, and those a block left for
 * the interpreter (see jit.h), run in the interpreter one at a time.
 *
 * Changed code (see jit.h): the blocks are listed by the 4 KB page where
 * they start, with a mask of the 64-byte chunks of each page that hold
 * translated code, so that a write finds the blocks it touches quickly.
 */
#include <setjmp.h>
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

static unsigned bucket_of(uint32_t pc)
{
    return (pc >> 1 ^ pc >> 13) & (JIT_HASH_SIZE - 1);
}

static unsigned ind_index(uint32_t pc)
{
    return (pc >> 1) & (JIT_IND_SIZE - 1);
}

bool jit_available(void)
{
    return jit_backend_available();
}

/* ---- The map and the code memory -------------------------------------- */

/* Sets the map from the page table, and the pages with code back to RAM. */
static void rebuild_map(relevo_cpu *c, jit_state *j)
{
    for (uint32_t p = 0; p < RELEVO_PAGE_COUNT; p++) {
        uint8_t kind = c->pages[p];
        if (kind == PAGE_RAM_CODE)
            c->pages[p] = kind = RELEVO_PAGE_RAM;
        uint8_t v = 0;
        if (!page_direct_read(kind))
            v |= JMAP_NO_READ;
        if (kind != RELEVO_PAGE_RAM)
            v |= JMAP_NO_WRITE;
        memset(j->map + ((size_t)p << (RELEVO_PAGE_SHIFT - JIT_MAP_SHIFT)),
               v, (size_t)1 << (RELEVO_PAGE_SHIFT - JIT_MAP_SHIFT));
    }
}

static void ind_clear(jit_state *j)
{
    for (unsigned i = 0; i < JIT_IND_SIZE; i++) {
        j->ind[i].pc = 1;
        j->ind[i].code = NULL;
    }
}

static bool write_begin(jit_state *j)
{
    if (j->write_depth == 0 && !codemem_begin_write(&j->mem))
        return false;
    j->write_depth++;
    return true;
}

static void write_end(jit_state *j)
{
    if (--j->write_depth == 0)
        codemem_end_write(&j->mem);
}

/* Points the rel32 of a jump at `site` to the offset `to`. */
static void patch_jump(jit_state *j, uint32_t site, size_t to)
{
    int32_t rel = (int32_t)((int64_t)to - (int64_t)(site + 4));
    memcpy(j->mem.rw + site, &rel, 4);
}

static void free_state(jit_state *j)
{
    free(j->pool);
    free(j->links);
    free(j->page_head);
    free(j->page_chunks);
    free(j);
}

bool jit_create(relevo_cpu *c)
{
    if (!jit_backend_available())
        return false;
    jit_state *j = calloc(1, sizeof *j);
    if (j == NULL)
        return false;
    j->npages = (uint32_t)(c->mem_size >> JIT_MAP_SHIFT);
    j->pool = calloc(JIT_POOL_SIZE, sizeof *j->pool);
    j->links = calloc(JIT_LINK_SIZE, sizeof *j->links);
    if (j->npages) {
        j->page_head = calloc(j->npages, sizeof *j->page_head);
        j->page_chunks = calloc(j->npages, sizeof *j->page_chunks);
    }
    if (j->pool == NULL || j->links == NULL
        || (j->npages && (j->page_head == NULL || j->page_chunks == NULL))) {
        free_state(j);
        return false;
    }
    if (!codemem_create(&j->mem, JIT_CODE_SIZE)) {
        free_state(j);
        return false;
    }
    j->links_used = 1;
    j->has_movbe = jit_backend_has_movbe();
    j->movbe = j->has_movbe;
    j->has_lahf = jit_backend_has_lahf();
    j->lazy = j->has_lahf;
    bool ok = write_begin(j);
    ok = ok && jit_backend_init(c, j);
    if (j->write_depth)
        write_end(j);
    if (!ok) {
        codemem_destroy(&j->mem);
        free_state(j);
        return false;
    }
    rebuild_map(c, j);
    ind_clear(j);
    c->jit = j;
    return true;
}

void jit_destroy(relevo_cpu *c)
{
    jit_state *j = c->jit;
    if (j == NULL)
        return;
    codemem_destroy(&j->mem);
    free_state(j);
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
    j->links_used = 1;
    j->links_free = 0;
    j->used = j->base_used;
    if (j->npages) {
        memset(j->page_head, 0, j->npages * sizeof *j->page_head);
        memset(j->page_chunks, 0, j->npages * sizeof *j->page_chunks);
    }
    ind_clear(j);
    rebuild_map(c, j);
    j->stats.flushes++;
}

/* ---- Links ------------------------------------------------------------ */

static void link_block(jit_state *j, uint32_t site, jit_block *b)
{
    if (b->code == NULL)
        return;
    uint32_t k = j->links_free;
    if (k != 0)
        j->links_free = j->links[k].next;
    else if (j->links_used < JIT_LINK_SIZE)
        k = j->links_used++;
    else
        return;
    if (!write_begin(j)) {
        j->links[k].next = j->links_free;
        j->links_free = k;
        return;
    }
    patch_jump(j, site, (size_t)(b->code - j->mem.rx));
    write_end(j);
    j->links[k].next = b->links;
    j->links[k].site = site;
    b->links = k;
    j->stats.links++;
}

/* Puts back every jump chained to `b` (to the exit that follows it).
 * Returns false if the code memory cannot be written. */
static bool unlink_block(jit_state *j, jit_block *b)
{
    if (b->links == 0)
        return true;
    if (!write_begin(j))
        return false;
    uint32_t k = b->links;
    while (k != 0) {
        jit_link *l = &j->links[k];
        uint32_t next = l->next;
        patch_jump(j, l->site, l->site + 4);
        l->next = j->links_free;
        j->links_free = k;
        k = next;
    }
    write_end(j);
    b->links = 0;
    return true;
}

/* ---- Pages with code -------------------------------------------------- */

/* The chunks of page `p` that [lo, hi) covers. */
static uint64_t chunks_of(uint64_t lo, uint64_t hi, uint32_t p)
{
    uint64_t pb = (uint64_t)p << JIT_MAP_SHIFT, pe = pb + (1u << JIT_MAP_SHIFT);
    if (lo < pb)
        lo = pb;
    if (hi > pe)
        hi = pe;
    if (lo >= hi)
        return 0;
    unsigned first = (unsigned)((lo - pb) >> JIT_CHUNK_SHIFT);
    unsigned last = (unsigned)((hi - 1 - pb) >> JIT_CHUNK_SHIFT);
    uint64_t m = last == 63 ? ~0ull : (1ull << (last + 1)) - 1;
    return m & ~((1ull << first) - 1);
}

static uint32_t first_page(const jit_block *b)
{
    return b->start >> JIT_MAP_SHIFT;
}

static uint32_t last_page(const jit_block *b)
{
    return (uint32_t)(((uint64_t)b->start + b->len - 1) >> JIT_MAP_SHIFT);
}

static void mark_block(relevo_cpu *c, jit_state *j, jit_block *b)
{
    uint32_t p0 = first_page(b);
    if (b->len == 0 || p0 >= j->npages)
        return;
    b->page_next = j->page_head[p0];
    j->page_head[p0] = b;
    b->listed = true;
    if (b->rom)
        return;
    for (uint32_t p = p0; p <= last_page(b) && p < j->npages; p++) {
        j->page_chunks[p] |= chunks_of(b->start, (uint64_t)b->start + b->len,
                                       p);
        j->map[p] |= JMAP_CODE;
        uint32_t pg = p >> (RELEVO_PAGE_SHIFT - JIT_MAP_SHIFT);
        if (c->pages[pg] == RELEVO_PAGE_RAM)
            c->pages[pg] = PAGE_RAM_CODE;
    }
}

/* The chunks of page `p` with code, from the blocks listed in it and in
 * the page before (whose blocks may run into it). */
static void recompute_page(jit_state *j, uint32_t p)
{
    uint64_t m = 0;
    for (uint32_t q = p ? p - 1 : 0; q <= p; q++)
        for (jit_block *b = j->page_head[q]; b != NULL; b = b->page_next)
            if (!b->rom)
                m |= chunks_of(b->start, (uint64_t)b->start + b->len, p);
    j->page_chunks[p] = m;
    if (m == 0)
        j->map[p] &= (uint8_t)~JMAP_CODE;
}

static void unlist(jit_state *j, jit_block *b)
{
    if (!b->listed)
        return;
    jit_block **p = &j->page_head[first_page(b)];
    while (*p != b)
        p = &(*p)->page_next;
    *p = b->page_next;
    b->listed = false;
    if (b->rom)
        return;
    for (uint32_t q = first_page(b); q <= last_page(b) && q < j->npages; q++)
        recompute_page(j, q);
}

static void drop(relevo_cpu *c, jit_state *j, jit_block *b)
{
    jit_block **p = &j->hash[bucket_of(b->pc)];
    while (*p != b)
        p = &(*p)->next;
    *p = b->next;
    unlist(j, b);
    if (!unlink_block(j, b)) {
        /* Cannot put the jumps back: start again from nothing. */
        b->next = j->free_list;
        j->free_list = b;
        jit_flush(c);
        return;
    }
    jit_ind *e = &j->ind[ind_index(b->pc)];
    if (e->pc == b->pc) {
        e->pc = 1;
        e->code = NULL;
    }
    j->stats.dropped++;
    b->next = j->free_list;
    j->free_list = b;
}

static bool overlaps(const jit_block *b, uint64_t lo, uint64_t hi)
{
    return b->len != 0 && b->start < hi && lo < (uint64_t)b->start + b->len;
}

/* A listed block with code in [lo, hi) (masked addresses), or NULL. */
static jit_block *find_overlap(const jit_state *j, uint64_t lo, uint64_t hi)
{
    uint32_t p0 = (uint32_t)(lo >> JIT_MAP_SHIFT);
    uint32_t p1 = (uint32_t)((hi - 1) >> JIT_MAP_SHIFT);
    for (uint32_t p = p0 ? p0 - 1 : 0; p <= p1 && p < j->npages; p++)
        for (jit_block *b = j->page_head[p]; b != NULL; b = b->page_next)
            if (overlaps(b, lo, hi))
                return b;
    return NULL;
}

/* Whether a write to [lo, hi) may touch translated RAM code. */
static bool may_hit_code(const jit_state *j, uint64_t lo, uint64_t hi)
{
    uint32_t p1 = (uint32_t)((hi - 1) >> JIT_MAP_SHIFT);
    for (uint32_t p = (uint32_t)(lo >> JIT_MAP_SHIFT); p <= p1; p++)
        if (p < j->npages && (j->map[p] & JMAP_CODE)
            && (j->page_chunks[p] & chunks_of(lo, hi, p)))
            return true;
    return false;
}

static void invalidate_range(relevo_cpu *c, jit_state *j, uint64_t lo,
                             uint64_t hi)
{
    if (lo >= hi)
        return;
    if (((hi - lo) >> JIT_MAP_SHIFT) > 4096) {
        /* A large range: look at every block. */
        for (unsigned k = 0; k < JIT_HASH_SIZE; k++) {
            jit_block **p = &j->hash[k];
            while (*p != NULL) {
                jit_block *b = *p;
                if (overlaps(b, lo, hi)) {
                    uint64_t flushes = j->stats.flushes;
                    drop(c, j, b);
                    if (j->stats.flushes != flushes)
                        return;
                } else {
                    p = &b->next;
                }
            }
        }
        return;
    }
    jit_block *b;
    while ((b = find_overlap(j, lo, hi)) != NULL) {
        uint64_t flushes = j->stats.flushes;
        drop(c, j, b);
        if (j->stats.flushes != flushes)
            return;
    }
}

void jit_invalidate(relevo_cpu *c, uint32_t address, uint32_t size)
{
    jit_state *j = c->jit;
    if (j == NULL || size == 0)
        return;
    uint64_t lo = address & c->addr_mask;
    invalidate_range(c, j, lo, lo + size);
}

void jit_code_written(relevo_cpu *c, uint32_t addr, unsigned size)
{
    jit_state *j = c->jit;
    if (j == NULL)
        return;
    uint64_t lo = addr, hi = lo + size;
    if (may_hit_code(j, lo, hi))
        invalidate_range(c, j, lo, hi);
}

int jit_access_check(relevo_cpu *c, uint32_t addr, uint32_t size_write)
{
    unsigned size = size_write & 0xFFFF;
    bool write = (size_write >> 16) != 0;
    uint64_t lo = addr, hi = lo + size;
    if (hi - 1 > c->addr_mask)
        return 0;
    bool direct = true;
    for (uint64_t p = lo >> RELEVO_PAGE_SHIFT; p <= (hi - 1) >> RELEVO_PAGE_SHIFT;
         p++) {
        uint8_t kind = c->pages[p];
        if (kind == RELEVO_PAGE_NONE)
            return 0;
        if (write ? kind != RELEVO_PAGE_RAM && kind != PAGE_RAM_CODE
                  : !page_direct_read(kind))
            direct = false;
    }
    if (!direct)
        return 2;
    if (write && may_hit_code(c->jit, lo, hi)
        && find_overlap(c->jit, lo, hi) != NULL)
        return 0;
    return 1;
}

/* Runs the instruction at the PC in the interpreter, for a block that met
 * a host page (or a ROM write). An instruction with other memory operands
 * than the one checked may take a bus or address error: that goes through
 * a setjmp() of its own here, so a fault never unwinds the block. One with
 * just that operand cannot fault (jit_access_check() found no missing page
 * and the block checked the alignment), and skips the setjmp. Host
 * functions may change the translations (relevo_invalidate(),
 * relevo_map(), writes into code); that takes effect at once, and the
 * block then leaves at once, through code that nothing changed (no
 * translation happens before the dispatcher runs again), so it never runs
 * stale code. Returns 0 if the block may go on to `next`, 1 if it must
 * leave for the dispatcher. */
int jit_host_step(relevo_cpu *c, uint32_t next, int may_fault)
{
    jit_state *j = c->jit;
    jit_ccr_flush(c);
    uint64_t flushes = j->stats.flushes, dropped = j->stats.dropped;
    int fault = 0;
    if (!may_fault) {
        cpu_interp_step(c);
        c->count--;             /* the dispatcher counts it, by the budget */
    } else {
        jmp_buf saved;
        memcpy(saved, c->fault_jmp, sizeof saved);
        if (setjmp(c->fault_jmp) == 0) {
            cpu_interp_step(c);
            c->count--;
        } else {
            /* A group 0 exception ended the instruction (relevo_run()). */
            fault = 1;
            if (c->in_insn) {
                c->in_insn = false;
                c->budget--;
            }
        }
        memcpy(c->fault_jmp, saved, sizeof saved);
    }
    j->stats.host++;
    return fault || c->exiting || c->halted || c->r.pc != next
        || j->stats.flushes != flushes || j->stats.dropped != dropped;
}

/* ---- Blocks ----------------------------------------------------------- */

static jit_block *lookup(jit_state *j, uint32_t pc)
{
    for (jit_block *b = j->hash[bucket_of(pc)]; b != NULL; b = b->next)
        if (b->pc == pc)
            return b;
    return NULL;
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
        /* Not translated: remember that, for the bytes that decided it
         * (at most an instruction's). */
        uint8_t kind = c->pages[b->start >> RELEVO_PAGE_SHIFT];
        uint32_t room = RELEVO_PAGE_SIZE - (b->start & (RELEVO_PAGE_SIZE - 1));
        b->len = page_direct_read(kind) ? (room < 22 ? room : 22) : 0;
        b->rom = kind == RELEVO_PAGE_ROM;
    } else {
        const jit_insn *last = &j->insns[n - 1];
        b->len = last->pc + last->len - pc;
        b->ninsns = (uint16_t)n;
        b->cut = n == max && !last->ends_block && max < JIT_MAX_INSNS;
        b->rom = rom;
        if (!write_begin(j)) {
            b->next = j->free_list;
            j->free_list = b;
            return NULL;
        }
        b->code = jit_backend_translate(c, j, n);
        write_end(j);
        if (b->code == NULL) {
            b->next = j->free_list;
            j->free_list = b;
            return NULL;
        }
        j->stats.blocks++;
        j->stats.insns += n;
    }
    unsigned k = bucket_of(pc);
    b->next = j->hash[k];
    j->hash[k] = b;
    mark_block(c, j, b);
    return b;
}

/* The block to run at the PC, or NULL for the interpreter. */
static jit_block *get_block(relevo_cpu *c, jit_state *j)
{
    jit_block *b = lookup(j, c->r.pc);
    if (b != NULL && b->cut && c->budget > b->ninsns) {
        drop(c, j, b);
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
    bool pc0 = (c->options & RELEVO_OPT_EXIT_ON_PC_ZERO) != 0;
    bool link = false;
    uint32_t link_site = 0, link_pc = 0;
    uint64_t link_flushes = 0;
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
                /* Never straight into PC 0 when the host waits for it. */
                if (!(pc0 && b->pc == 0)) {
                    if (link && link_pc == b->pc
                        && link_flushes == j->stats.flushes)
                        link_block(j, link_site, b);
                    jit_ind *e = &j->ind[ind_index(b->pc)];
                    e->pc = b->pc;
                    e->code = b->code;
                }
                link = false;
                j->stats.runs++;
                uint64_t budget = c->budget;
                uint64_t r = jit_backend_enter(c, j, b->code);
                c->count += budget - c->budget;
                if (c->jit_ccr != 0)
                    jit_ccr_flush(c);
                if ((uint32_t)r == JIT_EXIT_BAIL) {
                    j->stats.bails++;
                    j->force_interp = true;
                    continue;
                }
                if ((uint32_t)r == JIT_EXIT_CHAIN) {
                    link = true;
                    link_site = (uint32_t)(r >> 32);
                    link_pc = c->r.pc;
                    link_flushes = j->stats.flushes;
                }
                if (UNLIKELY(c->exiting))
                    return c->exit;
                if (UNLIKELY(c->halted))
                    return RELEVO_HALTED;
                if (UNLIKELY(pc0 && c->r.pc == 0))
                    return RELEVO_EXIT_PC_ZERO;
                continue;
            }
        }
        link = false;
        j->force_interp = false;
        j->stats.interp++;
        cpu_interp_step(c);
        if (UNLIKELY(c->exiting))
            return c->exit;
        if (UNLIKELY(pc0 && c->r.pc == 0))
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

void jit_set_lazy(relevo_cpu *c, bool on)
{
    if (c->jit == NULL)
        return;
    c->jit->lazy = on && c->jit->has_lahf;
    jit_flush(c);
}

void jit_ccr_flush(relevo_cpu *c)
{
    uint32_t r = c->jit_ccr;
    unsigned m = (r >> 16) & JF_NZVC, x = r >> 24;
    uint16_t sr = c->r.sr;
    if (m != 0) {
        unsigned fl = (r >> 8) & 0xFF;
        unsigned v = ((fl >> 7) & 1) << 3 | ((fl >> 6) & 1) << 2
                   | (r & 1) << 1 | (fl & 1);
        sr = (uint16_t)((sr & ~m) | (v & m));
    }
    if (x != 0)
        sr = (uint16_t)((sr & ~JF_X) | (x & 1) << 4);
    c->r.sr = sr;
    c->jit_ccr = 0;
}

bool jit_translatable(relevo_cpu *c, uint32_t pc)
{
    if (c->jit == NULL)
        return false;
    jit_insn in;
    bool rom = true;
    return jit_decode(c, pc, &in, &rom);
}
