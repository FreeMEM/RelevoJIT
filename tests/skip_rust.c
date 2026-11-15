/* SPDX-License-Identifier: MPL-2.0
 *
 * What the Rust library cannot run yet (docs/REESCRITURA.md): nothing since
 * RR3 (the interpreter, the FPU and the JIT are all there). The list stays
 * for what a later step may need to skip.
 */
#include <stddef.h>

#include "tests.h"

const test_skip tests_skipped[] = {
    { NULL, NULL }
};
