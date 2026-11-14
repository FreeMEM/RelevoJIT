/* SPDX-License-Identifier: MPL-2.0 */
#include "sst_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const sst_reg_names[SST_NREGS] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "usp", "ssp", "sr", "pc"
};

bool sst_open(sst_reader *r, const char *path)
{
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return false;
    }
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (buf == NULL) {
            fprintf(stderr, "%s: out of memory\n", path);
            fclose(f);
            return false;
        }
        len += fread(buf + len, 1, cap - len, f);
        if (len < cap)
            break;
        cap *= 2;
        char *nb = realloc(buf, cap);
        if (nb == NULL)
            free(buf);
        buf = nb;
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad) {
        fprintf(stderr, "%s: read error\n", path);
        free(buf);
        return false;
    }
    r->text = buf;
    r->len = len;
    json_init(&r->j, buf, len);
    return true;
}

void sst_open_text(sst_reader *r, const char *text, size_t len)
{
    memset(r, 0, sizeof *r);
    json_init(&r->j, text, len);
}

void sst_close(sst_reader *r)
{
    free(r->text);
    r->text = NULL;
}

void sst_test_free(sst_test *t)
{
    free(t->initial.ram);
    free(t->final.ram);
    memset(t, 0, sizeof *t);
}

static bool fail(json *j, const char *msg)
{
    if (j->err == NULL) {
        j->err = msg;
        j->err_offset = (size_t)(j->p - j->start);
    }
    return false;
}

static bool read_ram(json *j, sst_state *s)
{
    s->ram_count = 0;
    if (!json_array_begin(j))
        return false;
    while (json_array_next(j)) {
        uint32_t addr, val;
        if (!json_array_begin(j) || !json_array_next(j) || !json_u32(j, &addr)
            || !json_array_next(j) || !json_u32(j, &val))
            return false;
        if (json_array_next(j))
            return fail(j, "ram entry with more than two numbers");
        if (!json_ok(j))
            return false;
        if (val > 0xFF)
            return fail(j, "ram byte out of range");
        if (s->ram_count == s->ram_cap) {
            size_t cap = s->ram_cap ? s->ram_cap * 2 : 64;
            sst_byte *nr = realloc(s->ram, cap * sizeof *nr);
            if (nr == NULL)
                return fail(j, "out of memory");
            s->ram = nr;
            s->ram_cap = cap;
        }
        s->ram[s->ram_count].address = addr;
        s->ram[s->ram_count].value = (uint8_t)val;
        s->ram_count++;
    }
    return json_ok(j);
}

static bool read_state(json *j, sst_state *s)
{
    char key[16];
    uint32_t seen = 0;
    if (!json_object_begin(j))
        return false;
    while (json_object_next(j, key, sizeof key)) {
        int reg = -1;
        for (int i = 0; i < SST_NREGS; i++)
            if (strcmp(key, sst_reg_names[i]) == 0)
                reg = i;
        if (reg >= 0) {
            if (!json_u32(j, &s->reg[reg]))
                return false;
            seen |= 1u << reg;
        } else if (strcmp(key, "prefetch") == 0) {
            if (!json_array_begin(j) || !json_array_next(j)
                || !json_u32(j, &s->prefetch[0]) || !json_array_next(j)
                || !json_u32(j, &s->prefetch[1]))
                return false;
            if (json_array_next(j))
                return fail(j, "prefetch with more than two words");
            if (!json_ok(j))
                return false;
        } else if (strcmp(key, "ram") == 0) {
            if (!read_ram(j, s))
                return false;
        } else if (!json_skip(j)) {
            return false;
        }
    }
    if (!json_ok(j))
        return false;
    if (seen != (1u << SST_NREGS) - 1)
        return fail(j, "state without all registers");
    return true;
}

int sst_next(sst_reader *r, sst_test *t)
{
    json *j = &r->j;
    if (!r->started) {
        r->started = true;
        if (!json_array_begin(j))
            return -1;
    }
    if (!json_array_next(j))
        return json_ok(j) && json_end(j) ? 0 : -1;

    char key[32];
    t->name[0] = 0;
    t->cycles = 0;
    t->initial.ram_count = t->final.ram_count = 0;
    unsigned seen = 0;
    if (!json_object_begin(j))
        return -1;
    while (json_object_next(j, key, sizeof key)) {
        bool ok;
        if (strcmp(key, "name") == 0) {
            ok = json_string(j, t->name, sizeof t->name);
            seen |= 1;
        } else if (strcmp(key, "initial") == 0) {
            ok = read_state(j, &t->initial);
            seen |= 2;
        } else if (strcmp(key, "final") == 0) {
            ok = read_state(j, &t->final);
            seen |= 4;
        } else if (strcmp(key, "length") == 0) {
            ok = json_u32(j, &t->cycles);
        } else {
            ok = json_skip(j);  /* "transactions", and anything new */
        }
        if (!ok)
            return -1;
    }
    if (!json_ok(j))
        return -1;
    if (seen != 7) {
        fail(j, "test without name, initial or final");
        return -1;
    }
    return 1;
}
