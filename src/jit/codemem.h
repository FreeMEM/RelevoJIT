/* SPDX-License-Identifier: MPL-2.0
 *
 * Executable memory with W^X: no page is ever writable and executable at
 * once. On Linux the same memory is mapped twice, once read-write (where
 * the translator writes) and once read-execute (what runs), through a
 * memfd; if that is not possible, one mapping switches between RW and RX
 * with mprotect() around each write. Windows does the same with a section
 * mapped twice, or VirtualAlloc() and VirtualProtect(). Other systems need
 * their own implementation of these four functions (codemem.c).
 */
#ifndef RELEVO_JIT_CODEMEM_H
#define RELEVO_JIT_CODEMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct codemem {
    uint8_t *rw;        /* where code is written */
    uint8_t *rx;        /* where the same bytes run */
    size_t size;
    bool dual;          /* two mappings; else mprotect() toggling */
    int fd;             /* Linux: the memfd */
} codemem;

bool codemem_create(codemem *m, size_t size);
void codemem_destroy(codemem *m);

/* Brackets every write: after codemem_end_write() the bytes written may
 * run. Without the dual mapping these change the protection of all of it. */
bool codemem_begin_write(codemem *m);
void codemem_end_write(codemem *m);

#endif
