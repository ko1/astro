
SHELL := /bin/bash

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

RUNTIME=../../runtime
PRECISE_GC_DIR := $(RUNTIME)/precise_gc

# Absolute paths baked into ascheme_precise binary so that the AOT
# build (= astro_cs_build → cc on generated SD_*.c) can find node.h /
# precise_gc/gc_types.h regardless of where the binary is invoked from.
ASCHEME_PRECISE_DIR := $(abspath .)
RUNTIME_ABSPATH     := $(abspath $(RUNTIME))

# GC backend selector — identical to baruby_precise.  Phase 1 of the
# migration uses `none` (= libc malloc, no collection, no rooting needed).
# Later phases will enable mark / copy etc.
GC ?= none
GC_NUM_none             := 1
GC_NUM_mark             := 2
GC_NUM_mark_gen         := 3
GC_NUM_mark_gen_inc     := 4
GC_NUM_copy             := 5
GC_NUM_copy_gen         := 6
GC_NUM_copy_gen_inc     := 7
GC_NUM_mark_compact     := 8
GC_NUM_mark_compact_gen := 9
GC_NUM_bump             := 10
GC_NUM_mark_bump_gen    := 11
GC_NUM_immix            := 12
GC_NUM_immix_gen        := 13
GC_NUM_mark_bitmap_gen  := 14
GC_NUM_mark_card_gen    := 15
GC_NUM_mark_freelist    := 16
GC_NUM_copy_scramble    := 17
GC_NUM := $(GC_NUM_$(GC))
ifeq ($(GC_NUM),)
$(error Unknown GC backend "$(GC)")
endif

GC_SRC := $(PRECISE_GC_DIR)/gc_$(GC).c
# iter 76: gc_common.c は全 backend で必要 (aro_gc_reset_payload_header 等)。
GC_COMMON_SRC := $(PRECISE_GC_DIR)/gc_common.c

# Track current GC selection so a `make GC=foo` after `make GC=bar`
# actually rebuilds.
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC))
$(shell echo "$(GC)" > .built_gc)
endif

optflags=-O3

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

ascheme_precise: main.c parse.c node.c Makefile $(NODE_GENFILES) context.h node.h parse.h .built_gc
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -DBARUBY_GC=$(GC_NUM) \
	    -DASCHEME_PRECISE_DIR='"$(ASCHEME_PRECISE_DIR)"' \
	    -DASTRO_RUNTIME_DIR='"$(RUNTIME_ABSPATH)"' \
	    -o ascheme_precise main.c parse.c node.c $(GC_COMMON_SRC) $(GC_SRC) \
	    -ldl -lm -lgmp \
	    $(READLINE_LDFLAGS)

# Alias so existing test/ targets still work without changes.
ascheme: ascheme_precise
	ln -sf ascheme_precise ascheme

node_eval.c: ../../lib/* node.def ascheme_gen.rb
	ruby -I ../../lib -I . -r astrogen -r ascheme_gen \
	    -e 'ASTroGen.start([], node_def_class: AschemeNodeDef)'

# --- Tests ---
#
# Each test/*.scm is run; its output is compared to the matching
# test/*.expected file.  `make test` reports per-file pass/fail and a
# final tally.

TESTS = $(filter-out test/r5rs_chibi.scm,$(wildcard test/*.scm))

test: ascheme test/r5rs_chibi.scm
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.scm}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(./ascheme -q "$$t" 2>&1); \
	  want=$$(cat "$$exp"); \
	  if [ "$$out" = "$$want" ]; then \
	    p=$$((p+1)); printf "  OK  %s\n" "$$t"; \
	  else \
	    f=$$((f+1)); \
	    printf "FAIL  %s\n" "$$t"; \
	    diff <(printf "%s" "$$want") <(printf "%s" "$$out") | head -20 | sed 's/^/      /'; \
	  fi; \
	done; \
	echo "---"; \
	echo "passed=$$p  failed=$$f  total=$$((p+f))"; \
	echo "---"; \
	echo "chibi-scheme R5RS tests (errors below are expected — they"; \
	echo "exercise features ascheme doesn't implement: delay/force,"; \
	echo "dynamic-wind, user-defined macros)"; \
	./ascheme -q test/r5rs_chibi.scm 2>&1 | tail -1; \
	test $$f -eq 0

# Stress-mode regression: each ascheme test is re-run under
# BARUBY_GC_STRESS=1 (= GC on every alloc) with the currently selected
# backend.  Used to exercise the precise rooting in node.def / main.c
# helpers — bugs that are silent under low GC pressure surface
# immediately when stress kicks GC into every aro_gc_alloc call.
#
# Usage:
#   make test_stress              — default backend (= GC=mark)
#   make GC=copy test_stress      — copy collector + stress
#   make GC=copy_scramble test_stress  — scramble (= audit) backend
test_stress: ascheme
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.scm}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(BARUBY_GC_STRESS=1 ./ascheme -q "$$t" 2>&1 | grep -v '^\[baruby_gc'); \
	  want=$$(cat "$$exp"); \
	  if [ "$$out" = "$$want" ]; then \
	    p=$$((p+1)); printf "  OK  %s\n" "$$t"; \
	  else \
	    f=$$((f+1)); \
	    printf "FAIL  %s\n" "$$t"; \
	    diff <(printf "%s" "$$want") <(printf "%s" "$$out") | head -20 | sed 's/^/      /'; \
	  fi; \
	done; \
	echo "---"; \
	echo "passed=$$p  failed=$$f  total=$$((p+f))"; \
	echo "---"; \
	test $$f -eq 0

test/r5rs_chibi.scm: test/r5rs_adapt.rb .chibi/chibi-scheme-0.12/tests/r5rs-tests.scm
	@ruby test/r5rs_adapt.rb < .chibi/chibi-scheme-0.12/tests/r5rs-tests.scm > $@

.chibi/chibi-scheme-0.12/tests/r5rs-tests.scm:
	@bash bench/compare.sh >/dev/null 2>&1 || true

# --- Benchmarks ---

BENCH_SMALL = $(wildcard bench/small/*.scm)
BENCH_BIG   = $(wildcard bench/big/*.scm)

# `make bench` runs the small benchmark set across every available
# implementation (ascheme interp, AOT-first, AOT-cached, chibi-scheme,
# guile if installed) and prints one table.  First invocation fetches +
# builds chibi-scheme into .chibi/.
bench: ascheme
	@bash bench/compare.sh small

bench-big: ascheme
	@bash bench/compare.sh big

bench-all: ascheme
	@bash bench/compare.sh all

# Aliases — kept for older muscle memory.
compare:     bench
compare-big: bench-big
compare-all: bench-all

clean:
	rm -f ascheme $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
