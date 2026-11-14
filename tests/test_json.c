/* SPDX-License-Identifier: MPL-2.0 */
#include <string.h>

#include "check.h"
#include "json.h"
#include "tests.h"

static void init(json *j, const char *s)
{
    json_init(j, s, strlen(s));
}

void test_json_values(void)
{
    json j;
    char key[16], s[32];
    int64_t v;
    uint32_t u;
    init(&j, " { \"a\" : 12, \"b\":-5, \"s\":\"x\\\"\\\\\\n\\u00e9\\u0041\","
             "\"big\": 4294967295 , \"min\": -9223372036854775808 } ");
    CHECK(json_object_begin(&j));
    CHECK(json_object_next(&j, key, sizeof key));
    CHECK(strcmp(key, "a") == 0);
    CHECK(json_i64(&j, &v));
    CHECK_EQ(v, 12);
    CHECK(json_object_next(&j, key, sizeof key));
    CHECK(json_i64(&j, &v));
    CHECK(v == -5);
    CHECK(json_object_next(&j, key, sizeof key));
    CHECK(json_string(&j, s, sizeof s));
    CHECK(strcmp(s, "x\"\\\n\xC3\xA9" "A") == 0);
    CHECK(json_object_next(&j, key, sizeof key));
    CHECK(strcmp(key, "big") == 0);
    CHECK(json_u32(&j, &u));
    CHECK_EQ(u, 0xFFFFFFFFu);
    CHECK(json_object_next(&j, key, sizeof key));
    CHECK(json_i64(&j, &v));
    CHECK(v == INT64_MIN);
    CHECK(!json_object_next(&j, key, sizeof key));
    CHECK(json_ok(&j));
    CHECK(json_end(&j));
}

void test_json_nesting_and_skip(void)
{
    json j;
    int64_t v, sum = 0;
    int n = 0;
    init(&j, "[[1,2],[],{\"k\":[true,false,null,1.5e3,-2,\"]\"],\"o\":{}},3]");
    CHECK(json_array_begin(&j));
    CHECK(json_array_next(&j));
    CHECK(json_array_begin(&j));
    while (json_array_next(&j)) {
        CHECK(json_i64(&j, &v));
        sum += v;
        n++;
    }
    CHECK(json_ok(&j));
    CHECK_EQ(n, 2);
    CHECK_EQ(sum, 3);
    CHECK(json_array_next(&j));
    CHECK(json_array_begin(&j));
    CHECK(!json_array_next(&j));    /* empty */
    CHECK(json_ok(&j));
    CHECK(json_array_next(&j));
    CHECK(json_skip(&j));           /* the whole object */
    CHECK(json_array_next(&j));
    CHECK(json_i64(&j, &v));
    CHECK_EQ(v, 3);
    CHECK(!json_array_next(&j));
    CHECK(json_ok(&j));
    CHECK(json_end(&j));
}

static bool parses_array_of_ints(const char *s)
{
    json j;
    int64_t v;
    init(&j, s);
    if (!json_array_begin(&j))
        return false;
    while (json_array_next(&j))
        if (!json_i64(&j, &v))
            return false;
    return json_ok(&j) && json_end(&j);
}

void test_json_errors(void)
{
    CHECK(parses_array_of_ints("[1, 2, 3]"));
    CHECK(parses_array_of_ints("[]"));
    CHECK(!parses_array_of_ints("[1,]"));
    CHECK(!parses_array_of_ints("[,1]"));
    CHECK(!parses_array_of_ints("[1 2]"));
    CHECK(!parses_array_of_ints("[1"));
    CHECK(!parses_array_of_ints("[1.5]"));
    CHECK(!parses_array_of_ints("[1] x"));
    CHECK(!parses_array_of_ints("[99999999999999999999]"));

    json j;
    uint32_t u;
    char s[4], key[8];
    init(&j, "-1");
    CHECK(!json_u32(&j, &u));
    init(&j, "4294967296");
    CHECK(!json_u32(&j, &u));
    init(&j, "\"abcd\"");
    CHECK(!json_string(&j, s, sizeof s));   /* does not fit */
    init(&j, "\"ab");
    CHECK(!json_string(&j, s, sizeof s));
    init(&j, "{\"a\" 1}");
    CHECK(json_object_begin(&j));
    CHECK(!json_object_next(&j, key, sizeof key));
    CHECK(!json_ok(&j));
    CHECK(j.err_offset == 5);
    init(&j, "[tru]");
    CHECK(!json_skip(&j));
    /* After an error every call fails. */
    CHECK(!json_array_begin(&j));
}
