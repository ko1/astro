
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

optflags=-O3

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

ascheme: main.c node.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -o ascheme main.c node.c -ldl -lm -lgmp \
	    /usr/lib/x86_64-linux-gnu/libgc.so.1 \
	    $(READLINE_LDFLAGS)

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
