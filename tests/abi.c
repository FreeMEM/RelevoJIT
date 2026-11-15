/* SPDX-License-Identifier: MPL-2.0
 *
 * The Rust library's view of the structures in relevo.h against the C
 * compiler's: every size and field offset must be the same. The library
 * gives its own with relevo_abi_layout() (not part of the API).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "relevo.h"

/* Size or offset of `name` as the Rust library lays it out; UINT64_MAX if
 * it does not know the name. */
uint64_t relevo_abi_layout(const char *name);

typedef struct entry {
    const char *name;
    uint64_t value;
} entry;

#define SIZE(t) { "sizeof " #t, sizeof(t) }
#define ALIGN(t) { "alignof " #t, _Alignof(t) }
#define FIELD(t, f) { #t "." #f, offsetof(t, f) }

static const entry entries[] = {
    SIZE(relevo_model), SIZE(relevo_fpu), SIZE(relevo_engine),
    SIZE(relevo_trap_result), SIZE(relevo_page_kind), SIZE(relevo_status),

    SIZE(relevo_host), ALIGN(relevo_host),
    FIELD(relevo_host, ctx), FIELD(relevo_host, read),
    FIELD(relevo_host, write), FIELD(relevo_host, line_a),
    FIELD(relevo_host, line_f), FIELD(relevo_host, reset),

    SIZE(relevo_config), ALIGN(relevo_config),
    FIELD(relevo_config, model), FIELD(relevo_config, fpu),
    FIELD(relevo_config, engine), FIELD(relevo_config, memory),
    FIELD(relevo_config, memory_size), FIELD(relevo_config, address_mask),
    FIELD(relevo_config, options), FIELD(relevo_config, host),

    SIZE(relevo_fpreg), ALIGN(relevo_fpreg),
    FIELD(relevo_fpreg, mantissa), FIELD(relevo_fpreg, sign_exponent),

    SIZE(relevo_regs), ALIGN(relevo_regs),
    FIELD(relevo_regs, d), FIELD(relevo_regs, a), FIELD(relevo_regs, pc),
    FIELD(relevo_regs, sr), FIELD(relevo_regs, usp), FIELD(relevo_regs, isp),
    FIELD(relevo_regs, msp), FIELD(relevo_regs, vbr),
    FIELD(relevo_regs, cacr), FIELD(relevo_regs, caar),
    FIELD(relevo_regs, sfc), FIELD(relevo_regs, dfc), FIELD(relevo_regs, fp),
    FIELD(relevo_regs, fpcr), FIELD(relevo_regs, fpsr),
    FIELD(relevo_regs, fpiar), FIELD(relevo_regs, tc),
    FIELD(relevo_regs, itt0), FIELD(relevo_regs, itt1),
    FIELD(relevo_regs, dtt0), FIELD(relevo_regs, dtt1),
    FIELD(relevo_regs, mmusr), FIELD(relevo_regs, urp),
    FIELD(relevo_regs, srp), FIELD(relevo_regs, buscr),
    FIELD(relevo_regs, pcr),
};

int main(void)
{
    unsigned bad = 0, n = sizeof entries / sizeof entries[0];
    for (unsigned i = 0; i < n; i++) {
        uint64_t rust = relevo_abi_layout(entries[i].name);
        if (rust != entries[i].value) {
            printf("abi: %s is %llu in C, %llu in Rust\n", entries[i].name,
                   (unsigned long long)entries[i].value,
                   (unsigned long long)rust);
            bad++;
        }
    }
    printf("abi: %u/%u sizes and offsets match\n", n - bad, n);
    return bad != 0;
}
