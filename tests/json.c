/* SPDX-License-Identifier: MPL-2.0 */
#include "json.h"

#include <string.h>

void json_init(json *j, const char *text, size_t len)
{
    j->start = j->p = text;
    j->end = text + len;
    j->err = NULL;
    j->err_offset = 0;
}

static bool fail(json *j, const char *msg)
{
    if (j->err == NULL) {
        j->err = msg;
        j->err_offset = (size_t)(j->p - j->start);
    }
    return false;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Next non-blank character, or 0 at the end. */
static char peek(json *j)
{
    while (j->p < j->end && is_space(*j->p))
        j->p++;
    return j->p < j->end ? *j->p : 0;
}

static bool expect(json *j, char c)
{
    if (j->err != NULL)
        return false;
    if (peek(j) != c)
        return fail(j, c == ':' ? "expected ':'" : "unexpected character");
    j->p++;
    return true;
}

/* The last non-blank character before the cursor: '[' or '{' right after
 * an opening bracket, so the first element takes no comma. */
static char previous(const json *j)
{
    const char *q = j->p;
    while (q > j->start && is_space(q[-1]))
        q--;
    return q > j->start ? q[-1] : 0;
}

static bool next_element(json *j, char open, char close)
{
    if (j->err != NULL)
        return false;
    char c = peek(j);
    if (c == close) {
        j->p++;
        return false;
    }
    if (previous(j) == open)
        return c != 0 || fail(j, "unexpected end");
    if (c != ',')
        return fail(j, "expected ',' or closing bracket");
    j->p++;
    return true;
}

bool json_array_begin(json *j) { return expect(j, '['); }
bool json_object_begin(json *j) { return expect(j, '{'); }

bool json_array_next(json *j)
{
    return next_element(j, '[', ']');
}

bool json_object_next(json *j, char *key, size_t key_size)
{
    if (!next_element(j, '{', '}'))
        return false;
    return json_string(j, key, key_size) && expect(j, ':');
}

bool json_i64(json *j, int64_t *out)
{
    if (j->err != NULL)
        return false;
    char c = peek(j);
    bool neg = false;
    if (c == '-') {
        neg = true;
        j->p++;
    }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
        return fail(j, "expected a number");
    uint64_t v = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        unsigned d = (unsigned)(*j->p - '0');
        if (v > (UINT64_C(9223372036854775808) - d) / 10)
            return fail(j, "number too large");
        v = v * 10 + d;
        j->p++;
    }
    if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E'))
        return fail(j, "expected an integer");
    if (!neg && v > INT64_MAX)
        return fail(j, "number too large");
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

bool json_u32(json *j, uint32_t *out)
{
    int64_t v;
    if (!json_i64(j, &v))
        return false;
    if (v < 0 || v > UINT32_MAX)
        return fail(j, "number out of range");
    *out = (uint32_t)v;
    return true;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Reads a string into buf (or just walks it if buf is NULL). */
static bool string_into(json *j, char *buf, size_t size)
{
    if (!expect(j, '"'))
        return false;
    size_t n = 0;
    for (;;) {
        if (j->p >= j->end)
            return fail(j, "unterminated string");
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"')
            break;
        if (c < 0x20)
            return fail(j, "control character in string");
        char tmp[4];
        size_t len = 1;
        tmp[0] = (char)c;
        if (c == '\\') {
            if (j->p >= j->end)
                return fail(j, "unterminated string");
            char e = *j->p++;
            switch (e) {
            case '"': case '\\': case '/': tmp[0] = e; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'n': tmp[0] = '\n'; break;
            case 'r': tmp[0] = '\r'; break;
            case 't': tmp[0] = '\t'; break;
            case 'u': {
                if (j->end - j->p < 4)
                    return fail(j, "bad \\u escape");
                unsigned u = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hex(j->p[i]);
                    if (h < 0)
                        return fail(j, "bad \\u escape");
                    u = u << 4 | (unsigned)h;
                }
                j->p += 4;
                /* Basic plane only; surrogate pairs are not needed here. */
                if (u < 0x80) {
                    tmp[0] = (char)u;
                } else if (u < 0x800) {
                    tmp[0] = (char)(0xC0 | u >> 6);
                    tmp[1] = (char)(0x80 | (u & 0x3F));
                    len = 2;
                } else {
                    tmp[0] = (char)(0xE0 | u >> 12);
                    tmp[1] = (char)(0x80 | ((u >> 6) & 0x3F));
                    tmp[2] = (char)(0x80 | (u & 0x3F));
                    len = 3;
                }
                break;
            }
            default:
                return fail(j, "bad escape");
            }
        }
        if (buf != NULL) {
            if (n + len >= size)
                return fail(j, "string too long");
            memcpy(buf + n, tmp, len);
        }
        n += len;
    }
    if (buf != NULL)
        buf[n] = 0;
    return true;
}

bool json_string(json *j, char *buf, size_t size)
{
    if (size == 0)
        return fail(j, "string too long");
    return string_into(j, buf, size);
}

static bool literal(json *j, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0)
        return fail(j, "unexpected character");
    j->p += n;
    return true;
}

static bool skip_depth(json *j, int depth)
{
    if (depth > 64)
        return fail(j, "nested too deep");
    switch (peek(j)) {
    case '[':
        j->p++;
        while (json_array_next(j))
            if (!skip_depth(j, depth + 1))
                return false;
        return json_ok(j);
    case '{':
        j->p++;
        while (next_element(j, '{', '}'))
            if (!string_into(j, NULL, 0) || !expect(j, ':')
                || !skip_depth(j, depth + 1))
                return false;
        return json_ok(j);
    case '"':
        return string_into(j, NULL, 0);
    case 't': return literal(j, "true");
    case 'f': return literal(j, "false");
    case 'n': return literal(j, "null");
    case 0:
        return fail(j, "unexpected end");
    default: {
        /* A number, fraction and exponent allowed. */
        const char *s = j->p;
        if (*j->p == '-')
            j->p++;
        while (j->p < j->end
               && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.'
                   || *j->p == 'e' || *j->p == 'E' || *j->p == '+'
                   || *j->p == '-'))
            j->p++;
        if (j->p == s || (j->p == s + 1 && *s == '-'))
            return fail(j, "unexpected character");
        return true;
    }
    }
}

bool json_skip(json *j)
{
    if (j->err != NULL)
        return false;
    return skip_depth(j, 0);
}

bool json_end(json *j)
{
    if (j->err != NULL)
        return false;
    return peek(j) == 0 || fail(j, "trailing characters");
}
