/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "codemem.h"

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)

#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
static int make_memfd(size_t size)
{
#if defined(MFD_CLOEXEC)
    int fd = memfd_create("relevo-jit", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#else
    (void)size;
    return -1;
#endif
}
#endif

bool codemem_create(codemem *m, size_t size)
{
    m->size = size;
    m->fd = -1;
    m->dual = false;
#if defined(__linux__)
    int fd = make_memfd(size);
    if (fd >= 0) {
        void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        0);
        void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd,
                        0);
        if (rw != MAP_FAILED && rx != MAP_FAILED) {
            m->rw = rw;
            m->rx = rx;
            m->fd = fd;
            m->dual = true;
            return true;
        }
        if (rw != MAP_FAILED)
            munmap(rw, size);
        if (rx != MAP_FAILED)
            munmap(rx, size);
        close(fd);
    }
#endif
    /* One mapping, RX except while the translator writes. */
    void *p = mmap(NULL, size, PROT_READ | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;
    m->rw = m->rx = p;
    return true;
}

void codemem_destroy(codemem *m)
{
    if (m->rx == NULL)
        return;
    munmap(m->rx, m->size);
    if (m->dual) {
        munmap(m->rw, m->size);
        close(m->fd);
    }
    m->rw = m->rx = NULL;
}

bool codemem_begin_write(codemem *m)
{
    if (m->dual)
        return true;
    return mprotect(m->rw, m->size, PROT_READ | PROT_WRITE) == 0;
}

void codemem_end_write(codemem *m)
{
    if (!m->dual)
        mprotect(m->rw, m->size, PROT_READ | PROT_EXEC);
}

#else   /* no implementation for this system yet (Windows: VirtualAlloc) */

bool codemem_create(codemem *m, size_t size)
{
    (void)size;
    m->rw = m->rx = NULL;
    return false;
}

void codemem_destroy(codemem *m)
{
    (void)m;
}

bool codemem_begin_write(codemem *m)
{
    (void)m;
    return false;
}

void codemem_end_write(codemem *m)
{
    (void)m;
}

#endif
