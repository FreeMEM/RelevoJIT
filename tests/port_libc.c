/* SPDX-License-Identifier: MPL-2.0
 *
 * The port layer of the freestanding library (include/relevo_port.h) over
 * the C library, to test that library on Linux (make test-freestanding).
 * memcpy and the rest come from libc. The executable memory is a memfd
 * mapped twice at different addresses, as a host with W^X does it.
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "relevo_port.h"

void *relevo_port_alloc(size_t size)
{
    return calloc(1, size != 0 ? size : 1);
}

void relevo_port_free(void *p)
{
    free(p);
}

void *relevo_port_code_alloc(size_t size, void **exec)
{
    int fd = memfd_create("relevo-port", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);      /* the mappings keep the memory */
    if (rw == MAP_FAILED || rx == MAP_FAILED) {
        if (rw != MAP_FAILED)
            munmap(rw, size);
        if (rx != MAP_FAILED)
            munmap(rx, size);
        return NULL;
    }
    *exec = rx;
    return rw;
}

void relevo_port_code_free(void *writable, void *exec, size_t size)
{
    munmap(writable, size);
    if (exec != writable)
        munmap(exec, size);
}
