# RelevoJIT
#
# The library is the Rust crate (Cargo.toml, src/), built with cargo. The
# tests and tools in tests/ are C programs that link against it through its
# C interface (include/relevo.h) and a few test hooks (tests/hooks.h).
#
#   make                build the library, build/sst and the other tools
#   make test           the ABI check, unit tests, the oracle bench and the
#                       JIT against the interpreter (and the NOP
#                       SingleStepTests, if downloaded)
#   make data           download SingleStepTests/m68000 (tools/datos.sh)
#   make sst            run every SingleStepTests file (JIT=1: on the JIT)
#   make jitdiff        a longer run of the JIT against the interpreter
#   make sysdiff        the same on the machine state: every model and FPU,
#                       SR, VBR, frames, host traps, garbage as code
#   make bench          speed of the JIT against the interpreter
#   make oraculo        regenerate the oracle bench (tools/oraculo: FS-UAE, Emu68)
#   make CC=clang       any C11 compiler; objects are rebuilt when CC changes
#   make clean
#
# The C version the crate was translated from was retired in RR4
# (docs/REESCRITURA.md); it is in the history.

CARGO ?= cargo

CC ?= cc
CFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Werror -pedantic
ALL_CFLAGS := -std=c11 $(WARNINGS) $(CFLAGS)
CPPFLAGS += -Iinclude
DEPFLAGS = -MMD -MP
# libm: fenv.h in tests/test_fpu.c (the comparison with the x87). The rest:
# what the Rust standard library needs from the system (cargo rustc
# --release --crate-type staticlib -- --print native-static-libs). The
# Windows list (MSYS2, windows-gnu) has not been tried.
ifeq ($(OS),Windows_NT)
LDLIBS := -lm -lkernel32 -ladvapi32 -lntdll -luserenv -lws2_32 -ldbghelp
else
LDLIBS := -lm -lgcc_s -lutil -lrt -lpthread -ldl -lc
endif

BUILD := build
LIB := target/release/librelevo.a

TEST_COMMON := tests/json.c tests/sst_load.c tests/sst_run.c
UNIT_SRC := tests/main.c tests/test_api.c tests/test_exceptions.c \
            tests/test_010.c tests/test_020.c tests/test_fpu.c tests/test_json.c \
            tests/test_sst.c tests/test_jit.c $(TEST_COMMON)
SST_SRC := tests/sst.c $(TEST_COMMON)
ORACLE_SRC := tests/oraculo.c
JITDIFF_SRC := tests/jitdiff.c tests/gen68k.c
SYSDIFF_SRC := tests/sysdiff.c tests/gen68k.c
BENCH_SRC := tests/bench.c
ABI_SRC := tests/abi.c
ORACLE_DATA := $(wildcard tests/oraculo/*.txt)
SST_DATA := tests/datos/m68000

UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
SST_OBJ := $(SST_SRC:%.c=$(BUILD)/%.o)
ORACLE_OBJ := $(ORACLE_SRC:%.c=$(BUILD)/%.o)
JITDIFF_OBJ := $(JITDIFF_SRC:%.c=$(BUILD)/%.o)
SYSDIFF_OBJ := $(SYSDIFF_SRC:%.c=$(BUILD)/%.o)
BENCH_OBJ := $(BENCH_SRC:%.c=$(BUILD)/%.o)
ABI_OBJ := $(ABI_SRC:%.c=$(BUILD)/%.o)

# make sst JIT=1 runs SingleStepTests on the JIT engine.
SST_FLAGS := $(if $(JIT),-j,)

.PHONY: all test data sst oraculo jitdiff sysdiff bench clean FORCE

all: $(LIB) $(BUILD)/run_tests $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/jitdiff \
     $(BUILD)/sysdiff $(BUILD)/bench $(BUILD)/abi

# Rebuild everything when the compiler or its flags change.
$(BUILD)/compiler: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' | cmp -s - $@ \
	    || echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' > $@

$(BUILD)/%.o: %.c $(BUILD)/compiler
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

# cargo knows what is out of date; it leaves the library alone if nothing is.
$(LIB): FORCE
	$(CARGO) build --release --quiet

$(BUILD)/run_tests: $(UNIT_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(UNIT_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/sst: $(SST_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SST_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/oraculo: $(ORACLE_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(ORACLE_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/jitdiff: $(JITDIFF_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(JITDIFF_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/sysdiff: $(SYSDIFF_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SYSDIFF_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/bench: $(BENCH_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(BENCH_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/abi: $(ABI_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(ABI_OBJ) $(LIB) $(LDLIBS)

test: $(BUILD)/abi $(BUILD)/run_tests $(BUILD)/sst $(BUILD)/oraculo \
      $(BUILD)/jitdiff $(BUILD)/sysdiff
	$(BUILD)/abi
	$(BUILD)/run_tests
	$(BUILD)/oraculo $(ORACLE_DATA)
	$(BUILD)/oraculo -j $(ORACLE_DATA)
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 1
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68020 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68000 -n 5000 -k 12 -x
	$(BUILD)/jitdiff -m 68020 -n 5000 -k 16 -e
	$(BUILD)/jitdiff -m 68040 -n 5000 -k 64
	$(BUILD)/sysdiff -a -n 1000 -k 32
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(BUILD)/sst $(SST_DATA)/NOP.json'; \
	    $(BUILD)/sst $(SST_DATA)/NOP.json; \
	    $(BUILD)/sst -j $(SST_DATA)/NOP.json; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi

# Many more random cases, each model, both ways of swapping bytes and of
# keeping the condition codes.
jitdiff: $(BUILD)/jitdiff
	for m in 68000 68010 68020 68030 68040; do \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 1 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 16 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 16 -x || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 16 -e || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 64 || exit 1; \
	done

# Many more cases on the machine state, every model with and without its
# FPUs; the last run with nothing but random words as code.
sysdiff: $(BUILD)/sysdiff
	$(BUILD)/sysdiff -a -n 200000 -k 1
	$(BUILD)/sysdiff -a -n 100000 -k 16
	$(BUILD)/sysdiff -a -n 30000 -k 64
	$(BUILD)/sysdiff -a -n 50000 -k 16 -g 100

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

clean:
	rm -rf $(BUILD)
	$(CARGO) clean

-include $(UNIT_OBJ:.o=.d) $(SST_OBJ:.o=.d) $(ORACLE_OBJ:.o=.d) \
         $(JITDIFF_OBJ:.o=.d) $(SYSDIFF_OBJ:.o=.d) $(BENCH_OBJ:.o=.d) \
         $(ABI_OBJ:.o=.d)
