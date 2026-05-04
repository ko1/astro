SHELL := /bin/bash

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_head.h

RUNTIME = ../../runtime
ASTROLIB = ../../lib

optflags = -O2

READLINE_CFLAGS  := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

pystro: main.c node.c runtime.c lexer.c parser.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -o pystro main.c node.c -ldl -lm -lgmp \
	    /usr/lib/x86_64-linux-gnu/libgc.so.1 \
	    $(READLINE_LDFLAGS)

$(NODE_GENFILES): node.def $(ASTROLIB)/astrogen.rb pystro_gen.rb
	ruby -I $(ASTROLIB) -I . -r astrogen -r pystro_gen \
	    -e 'ASTroGen.start([], node_def_class: PystroNodeDef)'

# --- Tests ---
TESTS = $(wildcard test/*.py)

test: pystro
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.py}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(./pystro -q "$$t" 2>&1); \
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
	test $$f -eq 0

# --- Benchmarks ---
#
# Compares pystro (interpreter / AOT cached) against CPython 3 on the
# `bench/*.py` set, sized so each runs ~1 s on python3.  Best-of-3.
# `make aot-bench-warm` pre-bakes code_store/ once so the AOT-cached
# column reflects steady-state, not one-shot compile cost.
#
# Set CCACHE_DISABLE=1 in the environment if your local ccache config
# can't write to its cache dir (e.g. inside a sandbox).  Without this,
# `astro_cs_build` may report "make failed (exit 512)" on the first run.

BENCHES = $(sort $(wildcard bench/*.py))
PYTHON3 ?= python3

bench: pystro
	@printf "%-26s %10s %10s %10s\n" "bench" "python3" "interp" "AOT-cached"
	@for b in $(BENCHES); do \
	  py=$$(for i in 1 2 3; do /usr/bin/time -f "%e" $(PYTHON3) "$$b" 2>&1 1>/dev/null | tail -1; done | sort -n | head -1); \
	  rm -rf code_store; \
	  in=$$(for i in 1 2 3; do /usr/bin/time -f "%e" ./pystro --no-compile "$$b" 2>&1 1>/dev/null | tail -1; done | sort -n | head -1); \
	  rm -rf code_store; \
	  ./pystro -c "$$b" >/dev/null 2>&1; \
	  ac=$$(for i in 1 2 3; do /usr/bin/time -f "%e" ./pystro "$$b" 2>&1 1>/dev/null | tail -1; done | sort -n | head -1); \
	  printf "%-26s %10s %10s %10s\n" "$$(basename $$b)" "$${py}s" "$${in}s" "$${ac}s"; \
	done

clean:
	rm -f pystro $(NODE_GENFILES)
	rm -rf code_store
