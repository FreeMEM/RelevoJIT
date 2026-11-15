/* SPDX-License-Identifier: MPL-2.0
 *
 * <stdlib.h> for the freestanding build (make freestanding): only what the
 * library uses, over the host's relevo_port_alloc() and relevo_port_free()
 * (include/relevo_port.h).
 */
#ifndef RELEVO_PORT_STDLIB_H
#define RELEVO_PORT_STDLIB_H

#include <stddef.h>
#include <stdint.h>

#include "relevo_port.h"

static inline void *relevo_port_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size)
        return NULL;
    return relevo_port_alloc(n * size);
}

#define malloc(size) relevo_port_alloc(size)
#define calloc(n, size) relevo_port_calloc((n), (size))
#define free(p) relevo_port_free(p)

#endif
