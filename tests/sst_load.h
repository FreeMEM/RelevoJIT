/* SPDX-License-Identifier: MPL-2.0
 *
 * Reader for the SingleStepTests/m68000 JSON files (as written by
 * tools/sst2json.py, or upstream's decode.py: the shape is the same).
 *
 * A file is an array of tests:
 *
 *   {"name": "...", "initial": STATE, "final": STATE,
 *    "transactions": [...], "length": CYCLES}
 *
 *   STATE = {"d0".."d7", "a0".."a6", "usp", "ssp", "sr", "pc": number,
 *            "prefetch": [word, word], "ram": [[address, byte], ...]}
 *
 * "pc" is the address of the next prefetch: the instruction starts at pc - 4,
 * and prefetch[0] is its opcode word. Bus transactions are skipped for now.
 */
#ifndef RELEVO_TEST_SST_LOAD_H
#define RELEVO_TEST_SST_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"

enum {
    SST_D0 = 0, SST_A0 = 8, SST_USP = 15, SST_SSP, SST_SR, SST_PC,
    SST_NREGS
};

typedef struct sst_byte {
    uint32_t address;
    uint8_t value;
} sst_byte;

typedef struct sst_state {
    uint32_t reg[SST_NREGS];
    uint32_t prefetch[2];
    sst_byte *ram;
    size_t ram_count, ram_cap;
} sst_state;

typedef struct sst_test {
    char name[128];
    sst_state initial, final;
    uint32_t cycles;
} sst_test;

typedef struct sst_reader {
    char *text;
    size_t len;
    json j;
    bool started;
} sst_reader;

/* Reads the whole file. Returns false (with a message on stderr) if it
 * cannot. */
bool sst_open(sst_reader *r, const char *path);
/* Parses a buffer already in memory (not copied; must outlive the reader). */
void sst_open_text(sst_reader *r, const char *text, size_t len);
/* Next test: 1 if one was read, 0 at the end, -1 on a format error (see
 * r->j.err). `t` keeps its RAM buffers between calls. */
int sst_next(sst_reader *r, sst_test *t);
void sst_close(sst_reader *r);
void sst_test_free(sst_test *t);

/* Register names in SST_* order. */
extern const char *const sst_reg_names[SST_NREGS];

#endif
