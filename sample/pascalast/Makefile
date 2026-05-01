
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

optflags = -O3

.PHONY: test bench clean all aot-bench

# AOT round-trip for one benchmark.  Usage:
#   make aot-bench BENCH=fib
# 1. compile the program once with the plain interpreter to emit
#    node_specialized.c, then 2. rebuild pascalast against it, then
#    3. time the result.
BENCH ?= fib
aot-bench: pascalast
	./pascalast -q -c bench/$(BENCH).pas
	rm -f pascalast
	$(MAKE) -s pascalast
	@printf "AOT  "
	@/usr/bin/time -f "%e s" ./pascalast -q bench/$(BENCH).pas
	@printf "INTERP "
	@/usr/bin/time -f "%e s" ./pascalast -q --no-compile bench/$(BENCH).pas
	@:>node_specialized.c

pascalast: main.c node.c Makefile $(NODE_GENFILES) node_specialized.c context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall \
	    -Wno-unused-function -Wno-stringop-truncation \
	    -o pascalast main.c node.c -lm \
	    /usr/lib/x86_64-linux-gnu/libgc.so.1

node_eval.c: ../../lib/* node.def pascalast_gen.rb
	ruby -I ../../lib -I . -r astrogen -r pascalast_gen \
	    -e 'ASTroGen.start([], node_def_class: PascalastNodeDef)'

# An empty placeholder.  After running `./pascalast -c FILE.pas` this
# is overwritten with auto-generated specialized dispatchers; rebuild
# pascalast to pick them up.
node_specialized.c:
	touch -t 200001010000 node_specialized.c

# --- Tests ---

TESTS = $(wildcard test/[0-9][0-9]_*.pas)

test: pascalast
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.pas}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(./pascalast -q "$$t" 2>&1); \
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

# --- Benchmarks --- run each bench/*.pas under `time -p` ---

BENCHES = $(sort $(wildcard bench/*.pas))

bench: pascalast
	@for b in $(BENCHES); do \
	  printf "%-30s " "$${b##*/}"; \
	  /usr/bin/time -f "%e s   %M KB" -- ./pascalast -q "$$b" >/dev/null; \
	done

clean:
	rm -f pascalast $(NODE_GENFILES) node_specialized.c
