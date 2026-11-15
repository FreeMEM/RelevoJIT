# RelevoJIT
#
#   make                build/librelevo.a and build/sst
#   make test           unit tests, the oracle bench and the JIT against the
#                       interpreter (and the NOP SingleStepTests, if downloaded)
#   make data           download SingleStepTests/m68000 (tools/datos.sh)
#   make sst            run every SingleStepTests file (JIT=1: on the JIT)
#   make jitdiff        a longer run of the JIT against the interpreter
#   make bench          speed of the JIT against the interpreter
#   make oraculo        regenerate the oracle bench (tools/oraculo: FS-UAE, Emu68)
#   make CC=clang       any C11 compiler; objects are rebuilt when CC changes
#   make runbin         build/runbin: runs flat 68k binaries, prints a summary
#   make freestanding   build/freestanding/librelevo.a, without a C library
#                       (include/relevo_port.h); checks what it leaves undefined
#   make test-freestanding  the oracle bench, the JIT differential and the
#                       NOP SingleStepTests on that library, with a port
#                       layer over libc (tests/port_libc.c)
#   make clean

CC ?= cc
CFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Werror -pedantic
ALL_CFLAGS := -std=c11 $(WARNINGS) $(CFLAGS)
CPPFLAGS += -Iinclude
DEPFLAGS = -MMD -MP
# libm: the FPU's transcendental functions (long double)
LDLIBS := -lm

BUILD := build
LIB := $(BUILD)/librelevo.a

LIB_SRC := $(wildcard src/*.c src/interp/*.c src/jit/*.c src/jit/x86_64/*.c)
TEST_COMMON := tests/json.c tests/sst_load.c tests/sst_run.c
UNIT_SRC := tests/main.c tests/test_api.c tests/test_exceptions.c \
            tests/test_010.c tests/test_020.c tests/test_fpu.c tests/test_json.c tests/test_sst.c \
            tests/test_jit.c \
            $(TEST_COMMON)
SST_SRC := tests/sst.c $(TEST_COMMON)
ORACLE_SRC := tests/oraculo.c
JITDIFF_SRC := tests/jitdiff.c
BENCH_SRC := tests/bench.c
RUNBIN_SRC := tests/runbin.c

LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
SST_OBJ := $(SST_SRC:%.c=$(BUILD)/%.o)
ORACLE_OBJ := $(ORACLE_SRC:%.c=$(BUILD)/%.o)
JITDIFF_OBJ := $(JITDIFF_SRC:%.c=$(BUILD)/%.o)
BENCH_OBJ := $(BENCH_SRC:%.c=$(BUILD)/%.o)
RUNBIN_OBJ := $(RUNBIN_SRC:%.c=$(BUILD)/%.o)
ORACLE_DATA := $(wildcard tests/oraculo/*.txt)

SST_DATA := tests/datos/m68000

# make sst JIT=1 runs SingleStepTests on the JIT engine.
SST_FLAGS := $(if $(JIT),-j,)

.PHONY: all test data sst oraculo jitdiff bench runbin freestanding \
        check-freestanding test-freestanding clean FORCE

all: $(LIB) $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/jitdiff $(BUILD)/bench

# Rebuild everything when the compiler or its flags change.
$(BUILD)/compiler: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' | cmp -s - $@ \
	    || echo '$(CC) $(ALL_CFLAGS) $(CPPFLAGS)' > $@

$(BUILD)/%.o: %.c $(BUILD)/compiler
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

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

$(BUILD)/runbin: $(RUNBIN_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(RUNBIN_OBJ) $(LIB) $(LDLIBS)

runbin: $(BUILD)/runbin

test: $(BUILD)/run_tests $(BUILD)/sst $(BUILD)/oraculo $(BUILD)/jitdiff
	$(BUILD)/run_tests
	$(BUILD)/oraculo $(ORACLE_DATA)
	$(BUILD)/oraculo -j $(ORACLE_DATA)
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 1
	$(BUILD)/jitdiff -m 68000 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68020 -n 20000 -k 12
	$(BUILD)/jitdiff -m 68000 -n 5000 -k 12 -x
	$(BUILD)/jitdiff -m 68040 -n 5000 -k 64
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(BUILD)/sst $(SST_DATA)/NOP.json'; \
	    $(BUILD)/sst $(SST_DATA)/NOP.json; \
	    $(BUILD)/sst -j $(SST_DATA)/NOP.json; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi

# Many more random cases, each model, both ways of swapping bytes.
jitdiff: $(BUILD)/jitdiff
	for m in 68000 68010 68020 68040; do \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 1 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 200000 -k 16 || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 16 -x || exit 1; \
	    $(BUILD)/jitdiff -m $$m -n 50000 -k 64 || exit 1; \
	done

bench: $(BUILD)/bench
	$(BUILD)/bench

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
	python3 tools/oraculo/oraculo.py gen
	python3 tools/oraculo/oraculo.py fsuae --cpu 68020
	python3 tools/oraculo/oraculo.py fsuae --cpu 68040
	-python3 tools/oraculo/oraculo.py emu68
	$(BUILD)/oraculo $(ORACLE_DATA)

# ---- Without a C library ------------------------------------------------
#
# The same sources with -ffreestanding -nostdinc: the C library headers
# the code includes come from src/port/include, over the functions of
# include/relevo_port.h that the host defines; the compiler's own headers
# (stddef.h, stdint.h, stdatomic.h, cpuid.h...) stay. The JIT's executable
# memory comes from the host too (src/port/codemem.c instead of
# src/jit/codemem.c), and the transcendental functions are left out
# (RELEVO_NO_LIBM). FS_CFLAGS carries the target's flags, for example
#
#   make freestanding FS_CFLAGS='-O2 -mno-red-zone -mcmodel=kernel'

FS := $(BUILD)/freestanding
FS_LIB := $(FS)/librelevo.a
FS_CFLAGS ?= -O2
FS_SYSINC := $(shell $(CC) -print-file-name=include)
FS_ALL_CFLAGS := -std=c11 $(WARNINGS) -ffreestanding -nostdinc -isystem $(FS_SYSINC) \
                 -fno-stack-protector -DRELEVO_NO_LIBM $(FS_CFLAGS)
FS_CPPFLAGS := -Isrc/port/include -Iinclude
FS_SRC := $(filter-out src/jit/codemem.c,$(LIB_SRC)) src/port/codemem.c
FS_OBJ := $(FS_SRC:%.c=$(FS)/%.o)
# What the library may leave undefined (include/relevo_port.h), and the
# GOT symbol that the linker defines for position-independent code.
FS_ALLOWED := relevo_port_alloc relevo_port_free relevo_port_code_alloc \
              relevo_port_code_free memcpy memset memmove memcmp \
              __udivti3 __umodti3 __divti3 __modti3 _GLOBAL_OFFSET_TABLE_

$(FS)/compiler: FORCE
	@mkdir -p $(FS)
	@echo '$(CC) $(FS_ALL_CFLAGS) $(FS_CPPFLAGS)' | cmp -s - $@ \
	    || echo '$(CC) $(FS_ALL_CFLAGS) $(FS_CPPFLAGS)' > $@

$(FS)/%.o: %.c $(FS)/compiler
	@mkdir -p $(dir $@)
	$(CC) $(FS_ALL_CFLAGS) $(FS_CPPFLAGS) $(DEPFLAGS) -c $< -o $@

$(FS_LIB): $(FS_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

freestanding: $(FS_LIB)
	@$(MAKE) --no-print-directory check-freestanding

# Every symbol the library uses and does not define must be in FS_ALLOWED.
check-freestanding: $(FS_LIB)
	@nm -u $(FS_LIB) | awk 'NF == 2 { print $$2 }' | sort -u > $(FS)/undefined
	@nm --defined-only $(FS_LIB) | awk 'NF == 3 { print $$3 }' | sort -u > $(FS)/defined
	@comm -23 $(FS)/undefined $(FS)/defined > $(FS)/needs
	@bad=$$(for s in $$(cat $(FS)/needs); do \
	    case " $(FS_ALLOWED) " in *" $$s "*) ;; *) echo $$s ;; esac; done); \
	if [ -n "$$bad" ]; then \
	    echo "freestanding: undefined symbols outside include/relevo_port.h:"; \
	    echo "$$bad"; exit 1; \
	fi; \
	echo "freestanding: $(FS_LIB) needs only:" $$(cat $(FS)/needs)

# The freestanding library on Linux, with the port layer over libc.
$(FS)/port_libc.o: tests/port_libc.c $(BUILD)/compiler
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

$(FS)/oraculo: $(ORACLE_OBJ) $(FS)/port_libc.o $(FS_LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(FS)/jitdiff: $(JITDIFF_OBJ) $(FS)/port_libc.o $(FS_LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(FS)/sst: $(SST_OBJ) $(FS)/port_libc.o $(FS_LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(FS)/runbin: $(RUNBIN_OBJ) $(FS)/port_libc.o $(FS_LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

# Without the transcendental functions, the 68882's bench of them cannot
# pass (on the 68040 they are line F anyway).
FS_ORACLE_DATA := $(filter-out %/fputrans.68020.txt,$(ORACLE_DATA))

test-freestanding: freestanding $(FS)/oraculo $(FS)/jitdiff $(FS)/sst $(FS)/runbin
	$(FS)/oraculo $(FS_ORACLE_DATA)
	$(FS)/oraculo -j $(FS_ORACLE_DATA)
	$(FS)/jitdiff -m 68000 -n 20000 -k 12
	$(FS)/jitdiff -m 68020 -n 20000 -k 12
	$(FS)/jitdiff -m 68000 -n 5000 -k 12 -x
	$(FS)/jitdiff -m 68040 -n 5000 -k 64
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(FS)/sst $(SST_DATA)/NOP.json'; \
	    $(FS)/sst $(SST_DATA)/NOP.json && $(FS)/sst -j $(SST_DATA)/NOP.json; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(SST_OBJ:.o=.d) $(ORACLE_OBJ:.o=.d) \
         $(JITDIFF_OBJ:.o=.d) $(BENCH_OBJ:.o=.d) $(RUNBIN_OBJ:.o=.d) $(FS_OBJ:.o=.d) \
         $(FS)/port_libc.d
