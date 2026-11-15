/* SPDX-License-Identifier: MPL-2.0
 *
 * RelevoJIT without a C library: what the host provides.
 *
 * The normal build (make) uses the C library: malloc(), mmap() for the
 * JIT's executable memory, setjmp(), and libm for the FPU's transcendental
 * functions. `make freestanding` builds build/freestanding/librelevo.a
 * with -ffreestanding -nostdinc for hosts that have none of that (an
 * operating system's own processes, a kernel). That library calls the
 * functions below, which the host defines, and nothing else but:
 *
 *   - memcpy, memset, memmove and memcmp, with their standard meaning (a
 *     freestanding C compiler may call them on its own anyway);
 *   - the compiler's runtime helpers for 128-bit integers (__udivti3,
 *     __umodti3... from libgcc or compiler-rt; Rust's compiler_builtins
 *     has them).
 *
 * `make check-freestanding` lists what the library leaves undefined and
 * fails if it is anything else.
 *
 * Differences from the normal build:
 *
 *   - No transcendental functions (FSIN, FETOX, FLOGN... and FSINCOS):
 *     they take the line F exception (vector 11, after the host's line_f
 *     function) on every FPU, as on a 68040 or 68060 without its software
 *     package. The rest of the FPU is integer arithmetic and stays.
 *   - setjmp()/longjmp() inside relevo_run() are the compiler's
 *     __builtin_setjmp()/__builtin_longjmp() (GCC and Clang).
 *
 * The JIT needs x86-64; the interpreter builds for any target the compiler
 * has (FS_CFLAGS in the Makefile).
 */
#ifndef RELEVO_PORT_H
#define RELEVO_PORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `size` bytes, zeroed and aligned to 16 bytes, or NULL. */
void *relevo_port_alloc(size_t size);

/* Frees what relevo_port_alloc() returned. `p` may be NULL. */
void relevo_port_free(void *p);

/* Executable memory for the JIT: `size` bytes (a multiple of 64 KB, at
 * most 32 MB today), mapped twice. The function returns the writable view
 * (read and write, never executed) and puts in `*exec` the executable view
 * (read and execute, never written) of the same memory; or returns NULL.
 * Both start zeroed and aligned to at least 4 KB. A host without W^X may
 * return the same read-write-execute address in both. On x86-64 nothing
 * more is needed between writing and running: the instruction cache
 * follows the writes. */
void *relevo_port_code_alloc(size_t size, void **exec);

/* Frees memory from relevo_port_code_alloc(), with the same three
 * values. */
void relevo_port_code_free(void *writable, void *exec, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RELEVO_PORT_H */
