/* SPDX-License-Identifier: MPL-2.0
 *
 * <string.h> for the freestanding build: the four functions that any
 * freestanding C compiler may call, which the host defines.
 */
#ifndef RELEVO_PORT_STRING_H
#define RELEVO_PORT_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* -ffreestanding implies -fno-builtin: let the compiler inline the small
 * fixed-size copies again. */
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, c, n) __builtin_memset((d), (c), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))

#endif
