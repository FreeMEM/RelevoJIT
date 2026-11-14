/* SPDX-License-Identifier: MPL-2.0
 *
 * A small pull parser for JSON, enough for the test data. It walks a text
 * buffer in place and never allocates. Every function returns false on error
 * and leaves a message in j->err; after the first error all calls fail.
 *
 * Reading [1, 2] and {"a": 1}:
 *
 *     json_array_begin(&j);
 *     while (json_array_next(&j)) json_i64(&j, &v);
 *
 *     json_object_begin(&j);
 *     while (json_object_next(&j, key, sizeof key)) json_i64(&j, &v);
 *
 * json_array_next() and json_object_next() return false at the closing
 * bracket (consuming it) or on error; check json_ok() afterwards.
 */
#ifndef RELEVO_TEST_JSON_H
#define RELEVO_TEST_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct json {
    const char *start, *p, *end;
    const char *err;        /* NULL while all is well */
    size_t err_offset;
} json;

void json_init(json *j, const char *text, size_t len);
static inline bool json_ok(const json *j) { return j->err == NULL; }

bool json_array_begin(json *j);
bool json_array_next(json *j);
bool json_object_begin(json *j);
bool json_object_next(json *j, char *key, size_t key_size);

/* An integer without fraction or exponent. */
bool json_i64(json *j, int64_t *out);
/* An integer in 0..UINT32_MAX. */
bool json_u32(json *j, uint32_t *out);
/* A string, unescaped into `buf` (\uXXXX becomes UTF-8). Fails if it does not
 * fit with its terminating zero. */
bool json_string(json *j, char *buf, size_t size);
/* Skips any value. */
bool json_skip(json *j);
/* Succeeds if only whitespace is left. */
bool json_end(json *j);

#endif
