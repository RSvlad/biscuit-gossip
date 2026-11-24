# Kex-Fabrika (biscuitd) build.
#
#   make                 -> ./biscuitd         (release)
#   make debug           -> ./biscuitd-debug   (ASan + UBSan)
#   make test            -> unit tests         (SAN=1 adds ASan + UBSan)
#   make itest           -> integration scenarios tests/integration/s*.sh
#   make clean
#
# EXTRA_CFLAGS is appended to every build, e.g. make test EXTRA_CFLAGS=-DMAX_NODES=128

ifeq ($(origin CC),default)
CC := gcc
endif

STD     := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN    := -Wall -Wextra -Wpedantic -Werror
INC     := -Iinclude
DEPS    := -MMD -MP
BASE    := $(STD) $(WARN) $(INC) $(DEPS) $(EXTRA_CFLAGS)

REL_FLAGS := -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
SAN_FLAGS := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer

SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
TESTS    := $(wildcard tests/test_*.c)

REL_DIR := build/release
DBG_DIR := build/debug
REL_OBJS := $(patsubst src/%.c,$(REL_DIR)/%.o,$(SRCS))
DBG_OBJS := $(patsubst src/%.c,$(DBG_DIR)/%.o,$(SRCS))

# Unit tests: every tests/test_*.c is linked with all src/*.c except main.c.
ifeq ($(SAN),1)
TST_DIR   := build/test-san
TST_FLAGS := $(SAN_FLAGS)
else
TST_DIR   := build/test
TST_FLAGS := -g -O0
endif
TST_LIB_OBJS := $(patsubst src/%.c,$(TST_DIR)/%.o,$(LIB_SRCS))
TST_BINS     := $(patsubst tests/%.c,$(TST_DIR)/%,$(TESTS))

.PHONY: all debug test itest clean
.DEFAULT_GOAL := all
.SECONDARY: # keep intermediate objects (test .o files) between runs

all: biscuitd

debug: biscuitd-debug

biscuitd: $(REL_OBJS)
	$(CC) $(REL_FLAGS) -o $@ $^

biscuitd-debug: $(DBG_OBJS)
	$(CC) $(SAN_FLAGS) -o $@ $^

$(REL_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(REL_FLAGS) -c -o $@ $<

$(DBG_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(BASE) $(SAN_FLAGS) -c -o $@ $<

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
	    $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "all tests passed ($(words $(TST_BINS)) test programs)"

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

clean:
	rm -rf build biscuitd biscuitd-debug

-include $(wildcard $(REL_DIR)/*.d $(DBG_DIR)/*.d $(TST_DIR)/*.d $(TST_DIR)/tests/*.d)
