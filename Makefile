# RelevoJIT
#
#   make                build/librelevo.a and build/sst
#   make test           unit tests (and the NOP SingleStepTests, if downloaded)
#   make data           download SingleStepTests/m68000 (tools/datos.sh)
#   make sst            run every SingleStepTests file
#   make CC=clang       any C11 compiler; objects are rebuilt when CC changes
#   make clean

CC ?= cc
CFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Werror -pedantic
ALL_CFLAGS := -std=c11 $(WARNINGS) $(CFLAGS)
CPPFLAGS += -Iinclude
DEPFLAGS = -MMD -MP

BUILD := build
LIB := $(BUILD)/librelevo.a

LIB_SRC := $(wildcard src/*.c src/interp/*.c)
TEST_COMMON := tests/json.c tests/sst_load.c tests/sst_run.c
UNIT_SRC := tests/main.c tests/test_api.c tests/test_exceptions.c \
            tests/test_020.c tests/test_json.c tests/test_sst.c \
            $(TEST_COMMON)
SST_SRC := tests/sst.c $(TEST_COMMON)

LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
SST_OBJ := $(SST_SRC:%.c=$(BUILD)/%.o)

SST_DATA := tests/datos/m68000

.PHONY: all test data sst clean FORCE

all: $(LIB) $(BUILD)/sst

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
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(UNIT_OBJ) $(LIB)

$(BUILD)/sst: $(SST_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SST_OBJ) $(LIB)

test: $(BUILD)/run_tests $(BUILD)/sst
	$(BUILD)/run_tests
	@if [ -f $(SST_DATA)/NOP.json ]; then \
	    echo '$(BUILD)/sst $(SST_DATA)/NOP.json'; \
	    $(BUILD)/sst $(SST_DATA)/NOP.json; \
	else \
	    echo 'SingleStepTests not downloaded (make data): NOP check skipped'; \
	fi

data:
	tools/datos.sh

# Fails while instructions are missing; the summary is what matters.
sst: $(BUILD)/sst
	@if [ ! -d $(SST_DATA) ]; then echo 'run make data first'; exit 1; fi
	$(BUILD)/sst $(SST_DATA)/*.json

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(SST_OBJ:.o=.d)
