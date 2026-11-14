/* SPDX-License-Identifier: MPL-2.0
 *
 * Tests of the public API, memory and the first instructions.
 */
#include <string.h>

#include "check.h"
#include "relevo.h"
#include "tests.h"

#define MEM_SIZE 0x40000u   /* 256 KB: pages 0 to 3 */

static uint8_t mem[MEM_SIZE];

static relevo_cpu *make(relevo_model model, unsigned options,
                        const relevo_host *host)
{
    memset(mem, 0, sizeof mem);
    relevo_config cfg = {0};
    cfg.model = model;
    cfg.memory = mem;
    cfg.memory_size = sizeof mem;
    cfg.options = options;
    if (host)
        cfg.host = *host;
    return relevo_create(&cfg);
}

static void put16(uint32_t a, uint16_t v)
{
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}

static void put32(uint32_t a, uint32_t v)
{
    put16(a, (uint16_t)(v >> 16));
    put16(a + 2, (uint16_t)v);
}

static uint32_t get16(uint32_t a)
{
    return (uint32_t)mem[a] << 8 | mem[a + 1];
}

static uint32_t get32(uint32_t a)
{
    return get16(a) << 16 | get16(a + 2);
}

void test_config_validation(void)
{
    relevo_config cfg = {0};
    relevo_cpu *c = relevo_create(&cfg);
    CHECK(c != NULL);
    relevo_destroy(c);

    CHECK(relevo_create(NULL) == NULL);

    cfg.memory = mem;
    cfg.memory_size = 0x1000;           /* not a whole page */
    CHECK(relevo_create(&cfg) == NULL);

    cfg.memory = NULL;
    cfg.memory_size = MEM_SIZE;         /* size without memory */
    CHECK(relevo_create(&cfg) == NULL);

    cfg.memory = mem;
    cfg.engine = (relevo_engine)2;      /* no such engine */
    CHECK(relevo_create(&cfg) == NULL);
    cfg.engine = RELEVO_ENGINE_INTERP;

    cfg.fpu = RELEVO_FPU_68881;         /* not on a 68000 */
    CHECK(relevo_create(&cfg) == NULL);
    cfg.model = RELEVO_68030;
    c = relevo_create(&cfg);
    CHECK(c != NULL);
    relevo_destroy(c);

    cfg.fpu = RELEVO_FPU_INTERNAL;      /* only 68040 and 68060 */
    CHECK(relevo_create(&cfg) == NULL);
    cfg.model = RELEVO_68040;
    c = relevo_create(&cfg);
    CHECK(c != NULL);
    relevo_destroy(c);

    cfg.model = (relevo_model)99;
    CHECK(relevo_create(&cfg) == NULL);

    relevo_destroy(NULL);
}

void test_create_defaults(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    relevo_regs *r = relevo_get_regs(c);
    CHECK_EQ(r->sr, 0x2700);
    CHECK_EQ(r->pc, 0);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(r->d[i], 0);
        CHECK_EQ(r->a[i], 0);
    }
    CHECK_EQ(relevo_instructions(c), 0);

    put32(0, 0x00001234);   /* reset SSP */
    put32(4, 0x00002000);   /* reset PC */
    r->sr = 0;
    relevo_reset(c);
    CHECK_EQ(r->sr, 0x2700);
    CHECK_EQ(r->a[7], 0x1234);
    CHECK_EQ(r->isp, 0x1234);
    CHECK_EQ(r->pc, 0x2000);
    relevo_destroy(c);
}

void test_map_pages(void)
{
    relevo_cpu *c = make(RELEVO_68020, 0, NULL);
    CHECK_EQ(relevo_map(c, 0x8000, 0x10000, RELEVO_PAGE_RAM), -1);
    CHECK_EQ(relevo_map(c, 0x10000, 0x8000, RELEVO_PAGE_RAM), -1);
    CHECK_EQ(relevo_map(c, 0x30000, 0x20000, RELEVO_PAGE_RAM), -1);
    CHECK_EQ(relevo_map(c, 0x40000, 0x10000, RELEVO_PAGE_ROM), -1);
    CHECK_EQ(relevo_map(c, 0x20000, 0x20000, RELEVO_PAGE_ROM), 0);
    CHECK_EQ(relevo_map(c, 0xDF0000, 0x10000, RELEVO_PAGE_HOST), 0);
    CHECK_EQ(relevo_map(c, 0xFFFF0000u, 0x20000, RELEVO_PAGE_HOST), -1);
    CHECK_EQ(relevo_map(c, 0, (uint64_t)1 << 32, RELEVO_PAGE_NONE), 0);
    CHECK_EQ(relevo_map(c, 0, 0x10000, (relevo_page_kind)7), -1);

    /* After unmapping everything, memory is not touched. */
    mem[0x100] = 0x5A;
    CHECK_EQ(relevo_read(c, 0x100, 1), 0);
    relevo_write(c, 0x100, 0xFF, 1);
    CHECK_EQ(mem[0x100], 0x5A);
    relevo_destroy(c);
}

void test_memory_big_endian(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    relevo_write(c, 0x100, 0x11223344, 4);
    CHECK_EQ(mem[0x100], 0x11);
    CHECK_EQ(mem[0x101], 0x22);
    CHECK_EQ(mem[0x102], 0x33);
    CHECK_EQ(mem[0x103], 0x44);
    CHECK_EQ(relevo_read(c, 0x100, 4), 0x11223344);
    CHECK_EQ(relevo_read(c, 0x102, 2), 0x3344);
    CHECK_EQ(relevo_read(c, 0x101, 1), 0x22);
    relevo_write(c, 0x200, 0xABCD, 2);
    CHECK_EQ(get16(0x200), 0xABCD);
    relevo_write(c, 0x202, 0x1EF, 1);  /* only the low byte */
    CHECK_EQ(mem[0x202], 0xEF);
    CHECK_EQ(mem[0x203], 0);
    relevo_destroy(c);
}

typedef struct host_log {
    uint32_t addr[8], value[8];
    unsigned size[8];
    int writes, reads;
} host_log;

static uint32_t host_read(void *ctx, uint32_t addr, unsigned size)
{
    host_log *h = ctx;
    h->reads++;
    return (addr & 0xFF) | (size << 8);
}

static void host_write(void *ctx, uint32_t addr, uint32_t value,
                       unsigned size)
{
    host_log *h = ctx;
    if (h->writes < 8) {
        h->addr[h->writes] = addr;
        h->value[h->writes] = value;
        h->size[h->writes] = size;
    }
    h->writes++;
}

void test_memory_host_pages(void)
{
    host_log log = {0};
    relevo_host host = {0};
    host.ctx = &log;
    host.read = host_read;
    host.write = host_write;
    relevo_cpu *c = make(RELEVO_68000, 0, &host);

    CHECK_EQ(relevo_map(c, 0xDF0000, 0x10000, RELEVO_PAGE_HOST), 0);
    relevo_write(c, 0xDFF180, 0x0FFF, 2);
    CHECK_EQ(log.writes, 1);
    CHECK_EQ(log.addr[0], 0xDFF180);
    CHECK_EQ(log.value[0], 0x0FFF);
    CHECK_EQ(log.size[0], 2);
    CHECK_EQ(relevo_read(c, 0xDFF006, 2), 0x206);
    CHECK_EQ(relevo_read(c, 0xDFF010, 4), 0x410);
    CHECK_EQ(log.reads, 2);

    /* ROM: reads go to memory, writes are dropped. */
    mem[0x10000] = 0x4E;
    CHECK_EQ(relevo_map(c, 0x10000, 0x10000, RELEVO_PAGE_ROM), 0);
    relevo_write(c, 0x10000, 0, 1);
    CHECK_EQ(mem[0x10000], 0x4E);
    CHECK_EQ(relevo_read(c, 0x10000, 1), 0x4E);

    /* A host page without host functions. */
    relevo_destroy(c);
    c = make(RELEVO_68000, 0, NULL);
    CHECK_EQ(relevo_map(c, 0xDF0000, 0x10000, RELEVO_PAGE_HOST), 0);
    CHECK_EQ(relevo_read(c, 0xDFF000, 4), 0);
    relevo_write(c, 0xDFF000, 1, 4);
    relevo_destroy(c);
}

void test_memory_page_crossing(void)
{
    host_log log = {0};
    relevo_host host = {0};
    host.ctx = &log;
    host.read = host_read;
    host.write = host_write;
    relevo_cpu *c = make(RELEVO_68020, 0, &host);

    /* RAM to RAM */
    relevo_write(c, 0xFFFE, 0xA1B2C3D4, 4);
    CHECK_EQ(get32(0xFFFE), 0xA1B2C3D4);
    CHECK_EQ(relevo_read(c, 0xFFFE, 4), 0xA1B2C3D4);
    relevo_write(c, 0x1FFFF, 0x1234, 2);
    CHECK_EQ(get16(0x1FFFF), 0x1234);
    CHECK_EQ(relevo_read(c, 0x1FFFF, 2), 0x1234);

    /* RAM to host: the host sees byte accesses past the boundary. */
    CHECK_EQ(relevo_map(c, 0x30000, 0x10000, RELEVO_PAGE_HOST), 0);
    relevo_write(c, 0x2FFFE, 0x11223344, 4);
    CHECK_EQ(get16(0x2FFFE), 0x1122);
    CHECK_EQ(log.writes, 2);
    CHECK_EQ(log.addr[0], 0x30000);
    CHECK_EQ(log.value[0], 0x33);
    CHECK_EQ(log.size[0], 1);
    CHECK_EQ(log.addr[1], 0x30001);
    CHECK_EQ(log.value[1], 0x44);
    /* host_read gives (address & 0xFF) | size << 8; the CPU keeps only
     * the bytes it asked for. */
    CHECK_EQ(relevo_read(c, 0x2FFFE, 4), 0x11220001);
    CHECK_EQ(relevo_read(c, 0x30010, 1), 0x10);
    relevo_destroy(c);
}

void test_address_mask(void)
{
    /* 68000: 24-bit bus, the top byte is ignored. */
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    relevo_write(c, 0xFF000100u, 0xCAFE, 2);
    CHECK_EQ(get16(0x100), 0xCAFE);
    CHECK_EQ(relevo_read(c, 0x01000100, 2), 0xCAFE);
    relevo_destroy(c);

    /* 68020: 32 bits, 0x01000100 is another (unmapped) address. */
    c = make(RELEVO_68020, 0, NULL);
    relevo_write(c, 0x01000100, 0xCAFE, 2);
    CHECK_EQ(get16(0x100), 0);
    CHECK_EQ(relevo_read(c, 0x01000100, 2), 0);
    relevo_destroy(c);

    /* An explicit mask: accesses wrap around the end. */
    relevo_config cfg = {0};
    memset(mem, 0, sizeof mem);
    cfg.memory = mem;
    cfg.memory_size = MEM_SIZE;
    cfg.address_mask = MEM_SIZE - 1;
    c = relevo_create(&cfg);
    relevo_write(c, MEM_SIZE - 2, 0x55667788, 4);
    CHECK_EQ(get16(MEM_SIZE - 2), 0x5566);
    CHECK_EQ(get16(0), 0x7788);
    CHECK_EQ(relevo_read(c, MEM_SIZE - 2, 4), 0x55667788);
    relevo_destroy(c);
}

#define NOP 0x4E71
#define RTS 0x4E75

void test_nop_budget(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    for (uint32_t a = 0x1000; a < 0x1010; a += 2)
        put16(a, NOP);
    relevo_regs *r = relevo_get_regs(c);
    r->pc = 0x1000;
    r->sr = 0x0015;
    CHECK_EQ(relevo_run(c, 3), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x1006);
    CHECK_EQ(r->sr, 0x0015);    /* NOP leaves the flags alone */
    CHECK_EQ(relevo_instructions(c), 3);
    CHECK_EQ(relevo_run(c, 0), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x1006);
    CHECK_EQ(relevo_run(c, 2), RELEVO_BUDGET);
    CHECK_EQ(relevo_instructions(c), 5);
    relevo_destroy(c);
}

void test_rts_to_zero_exits(void)
{
    static const relevo_model models[] = {
        RELEVO_68000, RELEVO_68010, RELEVO_68020, RELEVO_68030,
        RELEVO_68040, RELEVO_68060
    };
    for (unsigned m = 0; m < sizeof models / sizeof models[0]; m++) {
        relevo_cpu *c = make(models[m], RELEVO_OPT_EXIT_ON_PC_ZERO, NULL);
        put16(0x1000, NOP);
        put16(0x1002, NOP);
        put16(0x1004, RTS);
        put32(0x8000, 0);   /* return address 0 on the stack */
        relevo_regs *r = relevo_get_regs(c);
        r->pc = 0x1000;
        r->a[7] = 0x8000;
        CHECK_EQ(relevo_run(c, 100), RELEVO_EXIT_PC_ZERO);
        CHECK_EQ(r->pc, 0);
        CHECK_EQ(r->a[7], 0x8004);
        CHECK_EQ(relevo_instructions(c), 3);
        relevo_destroy(c);
    }
}

void test_rts_to_zero_option_off(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    put16(0x1000, RTS);
    put32(0x8000, 0x2000);
    put32(0x8004, 0);
    put16(0x2000, NOP);
    put16(0x2002, RTS);
    relevo_regs *r = relevo_get_regs(c);
    r->pc = 0x1000;
    r->a[7] = 0x8000;
    CHECK_EQ(relevo_run(c, 3), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0);         /* went on: no exit without the option */
    CHECK_EQ(r->a[7], 0x8008);
    CHECK_EQ(relevo_instructions(c), 3);
    relevo_destroy(c);
}

void test_set_sr_switches_stacks(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    relevo_regs *r = relevo_get_regs(c);
    r->a[7] = 0x5000;           /* supervisor after create */
    r->usp = 0x6000;
    relevo_set_sr(c, 0x0000);
    CHECK_EQ(r->a[7], 0x6000);
    CHECK_EQ(r->isp, 0x5000);
    r->a[7] = 0x6100;
    relevo_set_sr(c, 0x3700);   /* M does not exist on the 68000 */
    CHECK_EQ(r->sr, 0x2700);
    CHECK_EQ(r->a[7], 0x5000);
    CHECK_EQ(r->usp, 0x6100);
    relevo_destroy(c);

    c = make(RELEVO_68020, 0, NULL);
    r = relevo_get_regs(c);
    r->a[7] = 0x5000;
    r->msp = 0x7000;
    relevo_set_sr(c, 0x3000);   /* S and M: master stack */
    CHECK_EQ(r->a[7], 0x7000);
    CHECK_EQ(r->isp, 0x5000);
    relevo_set_sr(c, 0x2000);
    CHECK_EQ(r->a[7], 0x5000);
    CHECK_EQ(r->msp, 0x7000);
    relevo_destroy(c);
}

void test_illegal_exception_68000(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    put32(4 * 4, 0x3000);       /* illegal instruction vector */
    put16(0x1000, 0x4AFC);      /* ILLEGAL */
    relevo_regs *r = relevo_get_regs(c);
    r->a[7] = 0x5000;
    relevo_set_sr(c, 0x0015);   /* user mode */
    r->a[7] = 0x6000;
    r->pc = 0x1000;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x3000);
    CHECK_EQ(r->sr, 0x2015);
    CHECK_EQ(r->a[7], 0x5000 - 6);
    CHECK_EQ(r->usp, 0x6000);
    CHECK_EQ(get16(0x4FFA), 0x0015);    /* stacked SR */
    CHECK_EQ(get32(0x4FFC), 0x1000);    /* stacked PC: the instruction */
    relevo_destroy(c);
}

void test_illegal_exception_68010(void)
{
    relevo_cpu *c = make(RELEVO_68010, 0, NULL);
    relevo_regs *r = relevo_get_regs(c);
    r->vbr = 0x20000;
    put32(0x20000 + 4 * 4, 0x3000);
    put16(0x1000, 0x0000 | 0x4AFC);
    r->a[7] = 0x5000;
    r->pc = 0x1000;
    r->sr = 0x2704;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x3000);
    CHECK_EQ(r->a[7], 0x5000 - 8);
    CHECK_EQ(get16(0x4FF8), 0x2704);    /* SR */
    CHECK_EQ(get32(0x4FFA), 0x1000);    /* PC */
    CHECK_EQ(get16(0x4FFE), 0x0010);    /* format 0, vector offset 0x10 */
    relevo_destroy(c);
}

typedef struct trap_ctx {
    relevo_trap_result result;
    uint32_t pc_seen;
    int calls;
} trap_ctx;

static relevo_trap_result trap_fn(void *ctx, relevo_cpu *cpu, uint16_t op)
{
    trap_ctx *t = ctx;
    relevo_regs *r = relevo_get_regs(cpu);
    t->calls++;
    t->pc_seen = r->pc;
    r->d[0] = op;
    return t->result;
}

void test_line_a_host(void)
{
    trap_ctx t = { RELEVO_TRAP_HANDLED, 0, 0 };
    relevo_host host = {0};
    host.ctx = &t;
    host.line_a = trap_fn;

    /* Handled: execution goes on after the opcode. */
    relevo_cpu *c = make(RELEVO_68000, 0, &host);
    put16(0x1000, 0xA123);
    put16(0x1002, NOP);
    relevo_regs *r = relevo_get_regs(c);
    r->pc = 0x1000;
    r->a[7] = 0x5000;
    CHECK_EQ(relevo_run(c, 2), RELEVO_BUDGET);
    CHECK_EQ(t.calls, 1);
    CHECK_EQ(t.pc_seen, 0x1002);
    CHECK_EQ(r->d[0], 0xA123);
    CHECK_EQ(r->pc, 0x1004);
    CHECK_EQ(r->a[7], 0x5000);
    relevo_destroy(c);

    /* Not handled: the line A exception, with the opcode's address. */
    t.result = RELEVO_TRAP_EXCEPTION;
    t.calls = 0;
    c = make(RELEVO_68000, 0, &host);
    put16(0x1000, 0xA123);
    put32(10 * 4, 0x3000);
    r = relevo_get_regs(c);
    r->pc = 0x1000;
    r->a[7] = 0x5000;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(t.calls, 1);
    CHECK_EQ(r->pc, 0x3000);
    CHECK_EQ(get32(0x5000 - 4), 0x1000);
    relevo_destroy(c);

    /* No host function: the exception too. */
    c = make(RELEVO_68000, 0, NULL);
    put16(0x1000, 0xAFFF);
    put32(10 * 4, 0x3000);
    r = relevo_get_regs(c);
    r->pc = 0x1000;
    r->a[7] = 0x5000;
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x3000);
    relevo_destroy(c);
}

void test_line_f_host_stop(void)
{
    trap_ctx t = { RELEVO_TRAP_STOP, 0, 0 };
    relevo_host host = {0};
    host.ctx = &t;
    host.line_f = trap_fn;
    relevo_cpu *c = make(RELEVO_68000, 0, &host);
    put16(0x1000, NOP);
    put16(0x1002, 0xF00D);
    put16(0x1004, NOP);
    relevo_regs *r = relevo_get_regs(c);
    r->pc = 0x1000;
    CHECK_EQ(relevo_run(c, 10), RELEVO_HOST_STOP);
    CHECK_EQ(r->pc, 0x1004);
    CHECK_EQ(r->d[0], 0xF00D);
    CHECK_EQ(relevo_instructions(c), 2);
    /* The next run goes on normally. */
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x1006);
    relevo_destroy(c);
}

void test_request_stop(void)
{
    relevo_cpu *c = make(RELEVO_68000, 0, NULL);
    put16(0x1000, NOP);
    relevo_regs *r = relevo_get_regs(c);
    r->pc = 0x1000;
    relevo_request_stop(c);
    CHECK_EQ(relevo_run(c, 10), RELEVO_HOST_STOP);
    CHECK_EQ(relevo_instructions(c), 0);
    CHECK_EQ(r->pc, 0x1000);
    CHECK_EQ(relevo_run(c, 1), RELEVO_BUDGET);
    CHECK_EQ(r->pc, 0x1002);
    relevo_set_ipl(c, 3);   /* accepted; interrupts come with R2 */
    relevo_invalidate(c, 0x1000, 2);
    relevo_destroy(c);
}

void test_independent_cpus(void)
{
    static uint8_t mem2[0x10000];
    relevo_cpu *a = make(RELEVO_68000, RELEVO_OPT_EXIT_ON_PC_ZERO, NULL);
    relevo_config cfg = {0};
    cfg.model = RELEVO_68020;
    cfg.memory = mem2;
    cfg.memory_size = sizeof mem2;
    relevo_cpu *b = relevo_create(&cfg);
    put16(0x1000, RTS);
    mem2[0x1000] = 0x4E;
    mem2[0x1001] = 0x71;
    relevo_get_regs(a)->pc = 0x1000;
    relevo_get_regs(a)->a[7] = 0x8000;
    relevo_get_regs(b)->pc = 0x1000;
    CHECK_EQ(relevo_run(a, 5), RELEVO_EXIT_PC_ZERO);
    CHECK_EQ(relevo_run(b, 1), RELEVO_BUDGET);
    CHECK_EQ(relevo_get_regs(a)->pc, 0);
    CHECK_EQ(relevo_get_regs(b)->pc, 0x1002);
    CHECK_EQ(relevo_get_regs(b)->a[7], 0);
    relevo_destroy(a);
    relevo_destroy(b);
}
