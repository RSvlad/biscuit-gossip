# Kex-Fabrika (biscuitd) build.
#
#   make                 -> ./biscuitd         (release)
#   make debug           -> ./biscuitd-debug   (ASan + UBSan)
#   make test            -> unit tests         (SAN=1 adds ASan + UBSan)
#   make itest           -> integration scenarios tests/integration/s*.sh
#   make itest-san       -> the same scenarios against ./biscuitd-debug
#   make valgrind        -> unit tests (and ./biscuitd-vg) under Valgrind, any finding fails
#   make cppcheck        -> static analysis, any finding fails
#   make lint            -> tests/lint.sh project rules (no malloc, no printf, ...)
#   make strict          -> extra warnings (-Wshadow ...), compile only, not part of check
#   make check           -> clean, all, test, test SAN=1, lint, cppcheck, valgrind
#   make clean
#
# EXTRA_CFLAGS is appended to every build, e.g. make test EXTRA_CFLAGS=-DMAX_NODES=128.
# The compiler flags are not tracked: run make clean when EXTRA_CFLAGS or CC changes.

ifeq ($(origin CC),default)
CC := gcc
endif

STD     := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN    := -Wall -Wextra -Wpedantic -Werror
INC     := -Iinclude
DEPS    := -MMD -MP
BASE    := $(STD) $(WARN) $(INC) $(DEPS) $(EXTRA_CFLAGS)

REL_FLAGS := -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
# no-recover: without it UBSan only prints "runtime error" and the test still passes
SAN_FLAGS := -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# ASan and Valgrind do not combine, so Valgrind gets its own sanitizer-free build
VG_FLAGS  := -g -O1
STRICT    := -Wshadow -Wformat=2 -Wcast-align -Wstrict-prototypes -Wmissing-prototypes

SAN_RUN_ENV := ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1 \
               UBSAN_OPTIONS=print_stacktrace=1
VG_OPTS := --leak-check=full --show-leak-kinds=all \
           --errors-for-leak-kinds=definite,indirect,possible \
           --track-origins=yes --error-exitcode=99

SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
TESTS    := $(wildcard tests/test_*.c)

REL_DIR := build/release
DBG_DIR := build/debug
VG_DIR  := build/valgrind
REL_OBJS := $(patsubst src/%.c,$(REL_DIR)/%.o,$(SRCS))
DBG_OBJS := $(patsubst src/%.c,$(DBG_DIR)/%.o,$(SRCS))
VG_OBJS  := $(patsubst src/%.c,$(VG_DIR)/%.o,$(SRCS))
VG_LIB_OBJS := $(patsubst src/%.c,$(VG_DIR)/%.o,$(LIB_SRCS))
VG_BINS  := $(patsubst tests/%.c,$(VG_DIR)/%,$(TESTS))

# Unit tests: every tests/test_*.c is linked with all src/*.c except main.c.
ifeq ($(SAN),1)
TST_DIR   := build/test-san
TST_FLAGS := $(SAN_FLAGS)
TST_ENV   := $(SAN_RUN_ENV)
else
TST_DIR   := build/test
TST_FLAGS := -g -O0
TST_ENV   :=
endif
TST_LIB_OBJS := $(patsubst src/%.c,$(TST_DIR)/%.o,$(LIB_SRCS))
TST_BINS     := $(patsubst tests/%.c,$(TST_DIR)/%,$(TESTS))

.PHONY: all debug test itest itest-san valgrind cppcheck lint strict check clean
.DEFAULT_GOAL := all
.SECONDARY: # keep intermediate objects (test .o files) between runs

all: biscuitd

debug: biscuitd-debug

biscuitd: $(REL_OBJS)
	$(CC) $(REL_FLAGS) -o $@ $^

biscuitd-debug: $(DBG_OBJS)
	$(CC) $(SAN_FLAGS) -o $@ $^

biscuitd-vg: $(VG_OBJS)
	$(CC) $(VG_FLAGS) -o $@ $^

$(REL_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(REL_FLAGS) -c -o $@ $<

$(DBG_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(SAN_FLAGS) -c -o $@ $<

$(VG_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(VG_FLAGS) -c -o $@ $<

$(VG_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(VG_FLAGS) -Itests -c -o $@ $<

$(VG_DIR)/test_%: $(VG_DIR)/tests/test_%.o $(VG_LIB_OBJS)
	$(CC) $(VG_FLAGS) -o $@ $^

$(TST_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(TST_FLAGS) -c -o $@ $<

$(TST_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(TST_FLAGS) -Itests -c -o $@ $<

$(TST_DIR)/test_%: $(TST_DIR)/tests/test_%.o $(TST_LIB_OBJS)
	$(CC) $(TST_FLAGS) -o $@ $^

test: $(TST_BINS)
	@fail=0; \
	for t in $(TST_BINS); do \
	    echo "== $$t"; \
	    $(TST_ENV) $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "all tests passed ($(words $(TST_BINS)) test programs)"

# Any Valgrind finding (error, definite/indirect/possible leak) makes the run fail.
# The daemon itself is exercised live by tests/integration/vg_smoke.sh (needs ./biscuitd-vg).
valgrind: $(VG_BINS) biscuitd-vg
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind: not installed"; exit 1; }
	@fail=0; \
	for t in $(VG_BINS); do \
	    echo "== valgrind $$t"; \
	    valgrind $(VG_OPTS) $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "VALGRIND FAILED"; exit 1; fi; \
	echo "valgrind: clean ($(words $(VG_BINS)) test programs)"

# Suppressions only inline: // cppcheck-suppress <id> ; reason
cppcheck:
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck: not installed"; exit 1; }
	cppcheck --enable=warning,style,performance,portability --std=c11 --inline-suppr \
	    --error-exitcode=1 -I include src tests

lint:
	bash tests/lint.sh --self-test
	bash tests/lint.sh

# Opt-in extra warnings, compile only (nothing is written to build/).
strict:
	@for f in $(SRCS) $(TESTS); do \
	    $(CC) $(STD) $(WARN) $(STRICT) $(INC) -Itests $(EXTRA_CFLAGS) -fsyntax-only $$f || exit 1; \
	done; \
	echo "strict: clean"

check:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test
	$(MAKE) test SAN=1
	$(MAKE) lint
	$(MAKE) cppcheck
	$(MAKE) valgrind

itest:
	@fail=0; ran=0; \
	for s in tests/integration/s*.sh; do \
	    [ -e "$$s" ] || continue; \
	    ran=$$((ran + 1)); \
	    echo "== $$s"; \
	    bash "$$s" || { fail=1; [ -n "$$ITEST_KEEP_GOING" ] || break; }; \
	done; \
	if [ $$ran -eq 0 ]; then echo "no integration scenarios yet"; fi; \
	exit $$fail

# Scenarios against the ASan/UBSan binary (tests/integration/lib.sh reads BIN).
itest-san: biscuitd-debug
	$(MAKE) itest BIN=./biscuitd-debug

clean:
	rm -rf build biscuitd biscuitd-debug biscuitd-vg

-include $(wildcard $(REL_DIR)/*.d $(DBG_DIR)/*.d $(VG_DIR)/*.d $(VG_DIR)/tests/*.d $(TST_DIR)/*.d $(TST_DIR)/tests/*.d)
