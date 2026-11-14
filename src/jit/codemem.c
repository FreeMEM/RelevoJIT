/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "codemem.h"

#if defined(_WIN32)

/* Windows: a pagefile-backed section mapped twice (a writable view and an
 * executable one), or else VirtualAlloc() memory switched between
 * PAGE_READWRITE and PAGE_EXECUTE_READ with VirtualProtect(). (Written
 * from the Win32 documentation; not run on Windows yet.) */
#include <windows.h>

bool codemem_create(codemem *m, size_t size)
{
    m->size = size;
    m->fd = -1;
    m->dual = false;
    m->rw = m->rx = NULL;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                  PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                  (DWORD)((unsigned long long)size >> 32),
                                  (DWORD)size, NULL);
    if (h != NULL) {
        void *rw = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, size);
        void *rx = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0,
                                 size);
        CloseHandle(h);     /* the views keep the section */
        if (rw != NULL && rx != NULL) {
            m->rw = rw;
            m->rx = rx;
            m->dual = true;
            return true;
        }
        if (rw != NULL)
            UnmapViewOfFile(rw);
        if (rx != NULL)
            UnmapViewOfFile(rx);
    }
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
                           PAGE_EXECUTE_READ);
    if (p == NULL)
        return false;
    m->rw = m->rx = p;
    return true;
}

void codemem_destroy(codemem *m)
{
    if (m->rx == NULL)
        return;
    if (m->dual) {
        UnmapViewOfFile(m->rx);
        UnmapViewOfFile(m->rw);
    } else {
        VirtualFree(m->rx, 0, MEM_RELEASE);
    }
    m->rw = m->rx = NULL;
}

bool codemem_begin_write(codemem *m)
{
    DWORD old;
    if (m->dual)
        return true;
    return VirtualProtect(m->rw, m->size, PAGE_READWRITE, &old) != 0;
}

void codemem_end_write(codemem *m)
{
    DWORD old;
    if (!m->dual)
        VirtualProtect(m->rx, m->size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), m->rx, m->size);
}

#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)

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

#else   /* no implementation for this system */

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
