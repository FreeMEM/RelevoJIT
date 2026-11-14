/* SPDX-License-Identifier: MPL-2.0
 *
 * The SingleStepTests reader and runner, on a hand-made sample (the real
 * files are not needed for `make test`).
 */
#include <string.h>

#include "check.h"
#include "sst_run.h"
#include "tests.h"

/* A NOP at 0x0DE9E8 in user mode, and an ILLEGAL at 0x2000 in supervisor
 * mode with its vector at 0x10 pointing to 0x400. */
static const char sample[] =
    "[\n"
    "{\"name\":\"000 NOP 4e71\",\"initial\":{\"d0\":1,\"d1\":2,\"d2\":3,"
    "\"d3\":4,\"d4\":5,\"d5\":6,\"d6\":7,\"d7\":8,\"a0\":9,\"a1\":10,"
    "\"a2\":11,\"a3\":12,\"a4\":13,\"a5\":14,\"a6\":15,\"usp\":7532898,"
    "\"ssp\":2766466,\"sr\":777,\"pc\":911852,\"prefetch\":[20081,55472],"
    "\"ram\":[[911848,78],[911849,113],[911850,216],[911851,176],"
    "[911852,50],[911853,107]]},"
    "\"final\":{\"d0\":1,\"d1\":2,\"d2\":3,\"d3\":4,\"d4\":5,\"d5\":6,"
    "\"d6\":7,\"d7\":8,\"a0\":9,\"a1\":10,\"a2\":11,\"a3\":12,\"a4\":13,"
    "\"a5\":14,\"a6\":15,\"usp\":7532898,\"ssp\":2766466,\"sr\":777,"
    "\"pc\":911854,\"prefetch\":[55472,12907],\"ram\":[[911848,78],"
    "[911849,113],[911850,216],[911851,176],[911852,50],[911853,107]]},"
    "\"transactions\":[[\"r\",4,2,911852,\".w\",12907,1,1]],\"length\":4},\n"
    "{\"name\":\"001 ILLEGAL 4afc\",\"initial\":{\"d0\":0,\"d1\":0,\"d2\":0,"
    "\"d3\":0,\"d4\":0,\"d5\":0,\"d6\":0,\"d7\":0,\"a0\":0,\"a1\":0,"
    "\"a2\":0,\"a3\":0,\"a4\":0,\"a5\":0,\"a6\":0,\"usp\":4096,"
    "\"ssp\":12288,\"sr\":9984,\"pc\":8196,\"prefetch\":[19196,20081],"
    "\"ram\":[[18,4],[19,0]]},"
    "\"final\":{\"d0\":0,\"d1\":0,\"d2\":0,\"d3\":0,\"d4\":0,\"d5\":0,"
    "\"d6\":0,\"d7\":0,\"a0\":0,\"a1\":0,\"a2\":0,\"a3\":0,\"a4\":0,"
    "\"a5\":0,\"a6\":0,\"usp\":4096,\"ssp\":12282,\"sr\":9984,"
    "\"pc\":1028,\"prefetch\":[0,0],\"ram\":[[18,4],[19,0],"
    "[12282,39],[12283,0],[12284,0],[12285,0],[12286,32],[12287,0]]},"
    "\"transactions\":[],\"length\":34}\n"
    "]\n";

void test_sst_reader(void)
{
    sst_reader rd;
    sst_test t;
    memset(&t, 0, sizeof t);
    sst_open_text(&rd, sample, strlen(sample));

    CHECK_EQ(sst_next(&rd, &t), 1);
    CHECK(strcmp(t.name, "000 NOP 4e71") == 0);
    CHECK_EQ(t.initial.reg[SST_D0], 1);
    CHECK_EQ(t.initial.reg[SST_A0 + 6], 15);
    CHECK_EQ(t.initial.reg[SST_USP], 7532898);
    CHECK_EQ(t.initial.reg[SST_SR], 777);
    CHECK_EQ(t.initial.reg[SST_PC], 911852);
    CHECK_EQ(t.initial.prefetch[0], 0x4E71);
    CHECK_EQ(t.initial.prefetch[1], 55472);
    CHECK_EQ(t.initial.ram_count, 6);
    CHECK_EQ(t.initial.ram[5].address, 911853);
    CHECK_EQ(t.initial.ram[5].value, 107);
    CHECK_EQ(t.final.reg[SST_PC], 911854);
    CHECK_EQ(t.cycles, 4);

    CHECK_EQ(sst_next(&rd, &t), 1);
    CHECK(strcmp(t.name, "001 ILLEGAL 4afc") == 0);
    CHECK_EQ(t.initial.ram_count, 2);
    CHECK_EQ(t.final.ram_count, 8);
    CHECK_EQ(t.cycles, 34);

    CHECK_EQ(sst_next(&rd, &t), 0);
    sst_close(&rd);

    /* A test without "final" is an error. */
    static const char bad[] = "[{\"name\":\"x\",\"initial\":{}}]";
    sst_open_text(&rd, bad, strlen(bad));
    CHECK_EQ(sst_next(&rd, &t), -1);
    sst_close(&rd);
    sst_test_free(&t);
}

void test_sst_run_nop(void)
{
    sst_env env;
    sst_reader rd;
    sst_test t;
    char why[256];
    memset(&t, 0, sizeof t);
    CHECK(sst_env_init(&env));
    sst_open_text(&rd, sample, strlen(sample));

    CHECK_EQ(sst_next(&rd, &t), 1);
    CHECK(sst_run_one(&env, &t, why, sizeof why));
    /* A wrong expectation is reported. */
    t.final.reg[SST_D0 + 3] = 99;
    CHECK(!sst_run_one(&env, &t, why, sizeof why));
    CHECK(strstr(why, "d3=00000004 (want 00000063)") != NULL);
    t.final.reg[SST_D0 + 3] = 4;
    t.final.ram[0].value = 0;
    CHECK(!sst_run_one(&env, &t, why, sizeof why));
    CHECK(strstr(why, "[0DE9E8]=4E (want 00)") != NULL);

    /* ILLEGAL: exception frame and vector, PC as the next prefetch. */
    CHECK_EQ(sst_next(&rd, &t), 1);
    CHECK(sst_run_one(&env, &t, why, sizeof why));

    sst_close(&rd);
    sst_test_free(&t);
    sst_env_free(&env);
}
