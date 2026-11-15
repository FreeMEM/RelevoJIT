# RelevoJIT
#
# The library is being rewritten in Rust (docs/REESCRITURA.md). The same C
# tests (tests/) run against either version:
#
#   IMPL=rust (default)  the Rust crate (Cargo.toml, src/), built with cargo
#   IMPL=c               the C version (c/src/), until the Rust one matches it
#
#   make                build the library, build/$(IMPL)/sst and the other tools
#   make test           unit tests, the oracle bench and the JIT against the
#                       interpreter (and the NOP SingleStepTests, if downloaded)
#   make data           download SingleStepTests/m68000 (tools/datos.sh)
#   make sst            run every SingleStepTests file (JIT=1: on the JIT)
#   make jitdiff        a longer run of the JIT against the interpreter
#   make bench          speed of the JIT against the interpreter
#   make crdiff         the Rust library against the C one, random code on
#                       every model (tests/crdiff.c; make c-crdiff: the C one
#                       against itself, which checks the tool)
#   make oraculo        regenerate the oracle bench (tools/oraculo: FS-UAE, Emu68)
#   make c-test         any target with c- in front: the same on the C version
#   make CC=clang       any C11 compiler; objects are rebuilt when CC changes
#   make clean
#
# What the Rust version does not have yet is skipped, with a notice: its
# unit tests in tests/skip_rust.c, and here the runs that need it.

IMPL ?= rust
CARGO ?= cargo

CC ?= cc
CFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Werror -pedantic
ALL_CFLAGS := -std=c11 $(WARNINGS) $(CFLAGS)
# The tests include the C library's internal headers (c/src) for a few
# constants and test hooks.
CPPFLAGS += -Iinclude -Ic/src
DEPFLAGS = -MMD -MP
# libm: the FPU's transcendental functions (long double)
LDLIBS := -lm

BUILD := build/$(IMPL)

TEST_COMMON := tests/json.c tests/sst_load.c tests/sst_run.c
SST_SRC := tests/sst.c $(TEST_COMMON)
ORACLE_SRC := tests/oraculo.c
JITDIFF_SRC := tests/jitdiff.c tests/gen68k.c
BENCH_SRC := tests/bench.c
CRDIFF_SRC := tests/crdiff.c tests/gen68k.c
# The C library with every global symbol renamed c_<name>, to link with
# either library in one program (crdiff).
C_LIB := build/c/librelevo.a
C_LIB_RENAMED := build/c/librelevo_c_.a
ORACLE_DATA := $(wildcard tests/oraculo/*.txt)
SST_DATA := tests/datos/m68000

ifeq ($(IMPL),c)
LIB := $(BUILD)/librelevo.a
LIB_SRC := $(wildcard c/src/*.c c/src/interp/*.c c/src/jit/*.c c/src/jit/x86_64/*.c)
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
UNIT_SRC := tests/main.c tests/test_api.c tests/test_exceptions.c \
            tests/test_010.c tests/test_020.c tests/test_json.c \
            tests/test_sst.c tests/test_jit.c tests/skip_c.c $(TEST_COMMON)
EXTRA_TOOLS :=
BENCH_FLAGS ?=
else ifeq ($(IMPL),rust)
LIB := target/release/librelevo.a
LIB_OBJ :=
# What the Rust standard library needs from the system (cargo rustc
# --release --crate-type staticlib -- --print native-static-libs).
LDLIBS += -lgcc_s -lutil -lrt -lpthread -ldl -lc
UNIT_SRC := tests/main.c tests/test_api.c tests/test_exceptions.c \
            tests/test_010.c tests/test_020.c tests/test_json.c \
            tests/test_sst.c tests/test_jit.c tests/skip_rust.c $(TEST_COMMON)
EXTRA_TOOLS := $(BUILD)/abi $(BUILD)/crdiff
# The interpreter only so far (RR2)
BENCH_FLAGS ?=
else
$(error IMPL must be rust or c)
endif

UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
SST_OBJ := $(SST_SRC:%.c=$(BUILD)/%.o)
ORACLE_OBJ := $(ORACLE_SRC:%.c=$(BUILD)/%.o)
JITDIFF_OBJ := $(JITDIFF_SRC:%.c=$(BUILD)/%.o)
BENCH_OBJ := $(BENCH_SRC:%.c=$(BUILD)/%.o)
CRDIFF_OBJ := $(CRDIFF_SRC:%.c=$(BUILD)/%.o)
ABI_OBJ := $(BUILD)/tests/abi.o

# make sst JIT=1 runs SingleStepTests on the JIT engine.
SST_FLAGS := $(if $(JIT),-j,)

.PHONY: all test data sst oraculo jitdiff crdiff bench clean FORCE

all: $(LIB) $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/jitdiff $(BUILD)/bench $(EXTRA_TOOLS)

# Rebuild everything when the compiler or its flags change.
$(BUILD)/compiler: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' | cmp -s - $@ \
	    || echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' > $@

$(BUILD)/%.o: %.c $(BUILD)/compiler
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

ifeq ($(IMPL),c)
$(LIB): $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $^
else
# cargo knows what is out of date; it leaves the library alone if nothing is.
$(LIB): FORCE
	$(CARGO) build --release --quiet
endif

$(BUILD)/run_tests: $(UNIT_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(UNIT_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/sst: $(SST_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SST_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/oraculo: $(ORACLE_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(ORACLE_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/jitdiff: $(JITDIFF_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(JITDIFF_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/bench: $(BENCH_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(BENCH_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/abi: $(ABI_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(ABI_OBJ) $(LIB) $(LDLIBS)

ifeq ($(IMPL),rust)
# The C library, built as make c-... builds it.
$(C_LIB): FORCE
	$(MAKE) IMPL=c CC='$(CC)' CFLAGS='$(CFLAGS)' $(C_LIB)
endif

$(C_LIB_RENAMED): $(C_LIB)
	nm -g --defined-only $< | awk 'NF == 3 { print $$3, "c_" $$3 }' \
	    | sort -u > $@.syms
	objcopy --redefine-syms=$@.syms $< $@

$(BUILD)/crdiff: $(CRDIFF_OBJ) $(LIB) $(C_LIB_RENAMED)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(CRDIFF_OBJ) $(LIB) \
	    $(C_LIB_RENAMED) $(LDLIBS)

ifeq ($(IMPL),c)
test: $(BUILD)/run_tests $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/jitdiff
	$(BUILD)/run_tests
	$(BUILD)/oraculo $(ORACLE_DATA)
	$(BUILD)/oraculo -j $(ORACLE_DATA)
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 1
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68020 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68000 -n 5000 -k 12 -x
	$(BUILD)/jitdiff -m 68020 -n 5000 -k 16 -e
	$(BUILD)/jitdiff -m 68040 -n 5000 -k 64
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(BUILD)/sst $(SST_DATA)/NOP.json'; \
	    $(BUILD)/sst $(SST_DATA)/NOP.json; \
	    $(BUILD)/sst -j $(SST_DATA)/NOP.json; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi
else
test: $(BUILD)/run_tests $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/abi $(BUILD)/crdiff
	$(BUILD)/abi
	$(BUILD)/run_tests
	$(BUILD)/oraculo $(ORACLE_DATA)
	$(BUILD)/crdiff -a -n 3000 -k 12
	@echo 'SKIPPED: the oracle bench on the JIT (oraculo -j) and jitdiff (the JIT: RR3)'
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(BUILD)/sst $(SST_DATA)/NOP.json'; \
	    $(BUILD)/sst $(SST_DATA)/NOP.json; \
	    echo 'SKIPPED: sst -j (the JIT: RR3)'; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi
endif

# Many more random cases, each model, both ways of swapping bytes and of
# keeping the condition codes.
ifeq ($(IMPL),c)
jitdiff: $(BUILD)/jitdiff
	for m in 68000 68010 68020 68030 68040; do \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 1 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 16 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 16 -x || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 16 -e || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 64 || exit 1; \
	done
else
jitdiff:
	@echo 'SKIPPED: jitdiff needs the JIT (RR3); make c-jitdiff runs it on the C version'
endif

# The Rust library against the C one: many random cases on every model.
crdiff: $(BUILD)/crdiff
	$(BUILD)/crdiff -a -n 300000 -k 1
	$(BUILD)/crdiff -a -n 200000 -k 16
	$(BUILD)/crdiff -a -n 50000 -k 64
	$(BUILD)/crdiff -a -n 50000 -k 16 -g 100

bench: $(BUILD)/bench
	$(BUILD)/bench $(BENCH_FLAGS)

data:
	tools/datos.sh

# Fails while instructions are missing; the summary is what matters.
sst: $(BUILD)/sst
	@if [ ! -d $(SST_DATA) ]; then echo 'run make data first'; exit 1; fi
	$(BUILD)/sst $(SST_FLAGS) $(SST_DATA)/*.json

# The bench against the oracle: generates the programs, runs them in FS-UAE
# with the real Kickstart as a 68020 and as a 68040 (DASH_KICKSTART) and
# writes tests/oraculo/*.txt; then compares Emu68 (jit-host) with the 68040.
oraculo: $(BUILD)/oraculo
	RELEVO_ORACULO=$(BUILD)/oraculo python3 tools/oraculo/oraculo.py gen
	RELEVO_ORACULO=$(BUILD)/oraculo python3 tools/oraculo/oraculo.py fsuae --cpu 68020
	RELEVO_ORACULO=$(BUILD)/oraculo python3 tools/oraculo/oraculo.py fsuae --cpu 68040
	-RELEVO_ORACULO=$(BUILD)/oraculo python3 tools/oraculo/oraculo.py emu68
	$(BUILD)/oraculo $(ORACLE_DATA)

# c-test, c-sst, c-bench...: the same target on the C version.
c-%:
	$(MAKE) IMPL=c $*

clean:
	rm -rf build
	$(CARGO) clean

-include $(LIB_OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(SST_OBJ:.o=.d) $(ORACLE_OBJ:.o=.d) \
         $(JITDIFF_OBJ:.o=.d) $(BENCH_OBJ:.o=.d) $(ABI_OBJ:.o=.d) \
         $(CRDIFF_OBJ:.o=.d)
