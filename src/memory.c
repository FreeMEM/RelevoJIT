/* SPDX-License-Identifier: MPL-2.0
 *
 * Memory accesses that cannot go straight to the memory block: host pages,
 * unmapped pages, ROM writes and accesses that cross a page boundary.
 */
#include "cpu.h"

static uint32_t read_in_page(relevo_cpu *c, uint32_t addr, unsigned size)
{
    switch (c->pages[addr >> RELEVO_PAGE_SHIFT]) {
    case RELEVO_PAGE_RAM:
    case RELEVO_PAGE_ROM: {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | c->mem[addr + i];
        return v;
    }
    case RELEVO_PAGE_HOST:
        if (c->host.read) {
            uint32_t v = c->host.read(c->host.ctx, addr, size);
            return size == 4 ? v : v & ((1u << (8 * size)) - 1);
        }
        return 0;
    default:
        /* TODO(R2): bus error */
        return 0;
    }
}

static void write_in_page(relevo_cpu *c, uint32_t addr, uint32_t v,
                          unsigned size)
{
    switch (c->pages[addr >> RELEVO_PAGE_SHIFT]) {
    case RELEVO_PAGE_RAM:
        for (unsigned i = 0; i < size; i++)
            c->mem[addr + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        return;
    case RELEVO_PAGE_HOST:
        if (c->host.write)
            c->host.write(c->host.ctx, addr, v, size);
        return;
    case RELEVO_PAGE_ROM:
        return;
    default:
        /* TODO(R2): bus error */
        return;
    }
}

/* `addr` is already masked. */
uint32_t mem_read_slow(relevo_cpu *c, uint32_t addr, unsigned size)
{
    if ((addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - size)
        return read_in_page(c, addr, size);
    /* Crosses a page: byte by byte, each address masked again (it may wrap
     * around the end of the address space). */
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v = v << 8 | read_in_page(c, (addr + i) & c->addr_mask, 1);
    return v;
}

void mem_write_slow(relevo_cpu *c, uint32_t addr, uint32_t v, unsigned size)
{
    if ((addr & (RELEVO_PAGE_SIZE - 1)) <= RELEVO_PAGE_SIZE - size) {
        write_in_page(c, addr, v, size);
        return;
    }
    for (unsigned i = 0; i < size; i++)
        write_in_page(c, (addr + i) & c->addr_mask,
                      (v >> (8 * (size - 1 - i))) & 0xFF, 1);
}
