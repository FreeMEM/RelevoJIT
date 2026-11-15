/* SPDX-License-Identifier: MPL-2.0
 *
 * <setjmp.h> for the freestanding build: the compiler's own non-local
 * jumps (GCC and Clang). relevo_run() is the only setjmp() and the jumps
 * come from the functions it calls, never from itself, as the builtins
 * require; the value is always 1.
 */
#ifndef RELEVO_PORT_SETJMP_H
#define RELEVO_PORT_SETJMP_H

typedef void *jmp_buf[5];

#define setjmp(buf) __builtin_setjmp(buf)
#define longjmp(buf, val) __builtin_longjmp((buf), 1)

#endif
