/* SPDX-License-Identifier: MPL-2.0
 *
 * The JIT's executable memory in the freestanding build (make
 * freestanding): the host maps it twice (relevo_port_code_alloc()), so it
 * is always the dual mapping and nothing changes around writes. Replaces
 * src/jit/codemem.c, which uses mmap().
 */
#include "relevo_port.h"

#include "../jit/codemem.h"

bool codemem_create(codemem *m, size_t size)
{
    void *rx = NULL;
    void *rw = relevo_port_code_alloc(size, &rx);
    m->size = size;
    m->fd = -1;
    m->dual = true;
    if (rw == NULL || rx == NULL) {
        if (rw != NULL)
            relevo_port_code_free(rw, rx, size);
        m->rw = m->rx = NULL;
        return false;
    }
    m->rw = rw;
    m->rx = rx;
    return true;
}

void codemem_destroy(codemem *m)
{
    if (m->rx == NULL)
        return;
    relevo_port_code_free(m->rw, m->rx, m->size);
    m->rw = m->rx = NULL;
}

bool codemem_begin_write(codemem *m)
{
    (void)m;
    return true;
}

void codemem_end_write(codemem *m)
{
    (void)m;
}
