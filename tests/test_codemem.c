/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT on code memory given by the host (relevo_config.code_memory), as
 * a system without the Rust standard library gives it (DashOS). Here the
 * host makes it as the library would on Linux: a memfd mapped twice, once
 * read-write and once read-execute. The tests check that the JIT uses the
 * host's functions as relevo.h says, and that it gives the same results.
 * They do nothing where there is no JIT or no memfd.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "relevo.h"
#include "tests.h"
#include "hooks.h"

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>

#define MEM_SIZE 0x100000u
#define CODE     0x10000u
#define STACK    0x8000u

typedef struct host_cm {
    int fd;
    size_t size;
    void *rw, *rx;
    unsigned maps, unmaps, begins, ends, refuse;
    size_t written;     /* bytes end_write() was told about */
    int bad;            /* a call out of order or out of range */
    int open;           /* between begin_write() and end_write() */
} host_cm;

static int cm_map(void *ctx, size_t size, void **rw, void **rx)
{
    host_cm *h = ctx;
    h->maps++;
    h->fd = memfd_create("relevo-test", MFD_CLOEXEC);
    if (h->fd < 0 || ftruncate(h->fd, (off_t)size) != 0)
        return -1;
    void *w = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    void *x = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, h->fd, 0);
    if (w == MAP_FAILED || x == MAP_FAILED)
        return -1;
    h->size = size;
    *rw = h->rw = w;
    *rx = h->rx = x;
    return 0;
}

static void cm_unmap(void *ctx, void *rw, void *rx, size_t size)
{
    host_cm *h = ctx;
    h->unmaps++;
    if (rw != h->rw || rx != h->rx || size != h->size)
        h->bad = 1;
    munmap(rw, size);
    munmap(rx, size);
    close(h->fd);
}

static int cm_begin(void *ctx)
{
    host_cm *h = ctx;
    if (h->open)
        h->bad = 1;
    if (h->refuse)
        return -1;
    h->begins++;
    h->open = 1;
    return 0;
}

static void cm_end(void *ctx, size_t offset, size_t size)
{
    host_cm *h = ctx;
    if (!h->open || offset > h->size || size > h->size - offset)
        h->bad = 1;
    h->open = 0;
    h->ends++;
    h->written += size;
}

static relevo_code_memory cm_table(host_cm *h, size_t size)
{
    memset(h, 0, sizeof *h);
    h->fd = -1;
    relevo_code_memory m = { 0 };
    m.ctx = h;
    m.size = size;
    m.map = cm_map;
    m.unmap = cm_unmap;
    m.begin_write = cm_begin;
    m.end_write = cm_end;
    return m;
}

static void put16(uint8_t *m, uint32_t a, uint32_t v)
{
    m[a] = (uint8_t)(v >> 8);
    m[a + 1] = (uint8_t)v;
}

/* `n` times addq.l #1,d0 and an rts at CODE: n / 64 blocks in a row. */
static relevo_cpu *straight(uint8_t *mem, unsigned n,
                            const relevo_code_memory *cm)
{
    for (unsigned i = 0; i < n; i++)
        put16(mem, CODE + 2 * i, 0x5280);
    put16(mem, CODE + 2 * n, 0x4E75);
    relevo_config cfg = { 0 };
    cfg.model = RELEVO_68020;
    cfg.engine = RELEVO_ENGINE_JIT;
    cfg.memory = mem;
    cfg.memory_size = MEM_SIZE;
    cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
    cfg.code_memory = cm;
    relevo_cpu *cpu = relevo_create(&cfg);
    if (cpu != NULL) {
        relevo_regs *r = relevo_get_regs(cpu);
        r->a[7] = STACK;
        r->pc = CODE;
    }
    return cpu;
}

/* The JIT writes through rw, runs from rx, and tells the host. */
void test_jit_host_code_memory(void)
{
    uint8_t *mem = calloc(1, MEM_SIZE);
    host_cm h;
    relevo_code_memory m = cm_table(&h, 0);
    relevo_cpu *cpu = straight(mem, 1000, &m);
    CHECK(cpu != NULL);
    if (cpu != NULL) {
        CHECK_EQ(h.maps, 1);
        CHECK_EQ(h.size, 32u << 20);    /* size 0: the default */
        CHECK_EQ(relevo_run(cpu, 100000), RELEVO_EXIT_PC_ZERO);
        CHECK_EQ(relevo_get_regs(cpu)->d[0], 1000);
        jit_stats s;
        jit_get_stats(cpu, &s);
        CHECK(s.blocks >= 16);
        CHECK_EQ(s.interp, 0);
        /* the trampoline at creation, and then every block */
        CHECK(h.begins >= 1 + s.blocks);
        CHECK_EQ(h.begins, h.ends);
        CHECK(h.written > 0);
        /* code changed by the host: translated again */
        put16(mem, CODE, 0x5480);       /* addq.l #2,d0 */
        relevo_invalidate(cpu, CODE, 2);
        relevo_regs *r = relevo_get_regs(cpu);
        r->d[0] = 0;
        r->a[7] = STACK;
        r->pc = CODE;
        CHECK_EQ(relevo_run(cpu, 100000), RELEVO_EXIT_PC_ZERO);
        CHECK_EQ(r->d[0], 1001);
        relevo_destroy(cpu);
        CHECK_EQ(h.unmaps, 1);
        CHECK(!h.bad);
    }
    free(mem);
}

/* A table relevo.h does not allow, or a map() that fails: no CPU. The
 * interpreter ignores the table. */
void test_jit_host_code_memory_bad(void)
{
    uint8_t *mem = calloc(1, MEM_SIZE);
    host_cm h;
    relevo_code_memory m = cm_table(&h, 4096);  /* less than 64 KB */
    CHECK(straight(mem, 4, &m) == NULL);
    m = cm_table(&h, 0x10800);                  /* not a multiple of 4 KB */
    CHECK(straight(mem, 4, &m) == NULL);
    m = cm_table(&h, 0);
    m.map = NULL;
    CHECK(straight(mem, 4, &m) == NULL);
    CHECK_EQ(h.maps, 0);
    m = cm_table(&h, (size_t)1 << 20);
    m.ctx = NULL;       /* cm_map() dereferences it: must not be called */
    relevo_config cfg = { 0 };
    cfg.memory = mem;
    cfg.memory_size = MEM_SIZE;
    cfg.code_memory = &m;
    relevo_cpu *cpu = relevo_create(&cfg);      /* the interpreter */
    CHECK(cpu != NULL);
    relevo_destroy(cpu);
    free(mem);
}

/* 64 KB of code memory for far more code: the JIT drops everything and
 * starts again each time it is full, with the same results. */
void test_jit_host_code_memory_small(void)
{
    uint8_t *mem = calloc(1, MEM_SIZE);
    host_cm h;
    relevo_code_memory m = cm_table(&h, 64u << 10);
    relevo_cpu *cpu = straight(mem, 30000, &m);
    CHECK(cpu != NULL);
    if (cpu != NULL) {
        CHECK_EQ(relevo_run(cpu, 100000), RELEVO_EXIT_PC_ZERO);
        CHECK_EQ(relevo_get_regs(cpu)->d[0], 30000);
        jit_stats s;
        jit_get_stats(cpu, &s);
        CHECK(s.flushes > 0);
        CHECK_EQ(s.interp, 0);
        relevo_destroy(cpu);
        CHECK(!h.bad);
    }
    free(mem);
}

/* A host that does not let the JIT write: everything runs in the
 * interpreter, and right. */
void test_jit_host_code_memory_refused(void)
{
    uint8_t *mem = calloc(1, MEM_SIZE);
    host_cm h;
    relevo_code_memory m = cm_table(&h, 0);
    h.refuse = 1;   /* even the trampoline */
    CHECK(straight(mem, 100, &m) == NULL);
    CHECK_EQ(h.unmaps, 1);
    m = cm_table(&h, 0);
    relevo_cpu *cpu = straight(mem, 100, &m);
    CHECK(cpu != NULL);
    if (cpu != NULL) {
        h.refuse = 1;
        CHECK_EQ(relevo_run(cpu, 100000), RELEVO_EXIT_PC_ZERO);
        CHECK_EQ(relevo_get_regs(cpu)->d[0], 100);
        jit_stats s;
        jit_get_stats(cpu, &s);
        CHECK_EQ(s.blocks, 0);
        CHECK_EQ(s.interp, 101);
        relevo_destroy(cpu);
        CHECK(!h.bad);
    }
    free(mem);
}

#else

void test_jit_host_code_memory(void) {}
void test_jit_host_code_memory_bad(void) {}
void test_jit_host_code_memory_small(void) {}
void test_jit_host_code_memory_refused(void) {}

#endif
