
all: asom

.PHONY: all run gen clean test bench

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

ASTROGEN = ruby -I ../../lib -r ./asom_gen \
               -e 'ASTroGen.start([], node_def_class: ASOMNodeDef)'

RUNTIME = ../../runtime

CC      ?= gcc
optflags?= -O2 -ggdb3
CFLAGS  := -I. -I$(RUNTIME) $(optflags) -Wall -Wno-unused-parameter -Wno-unused-function

SRCS = main.c node.c asom_runtime.c asom_loader.c asom_parse.c asom_primitives.c

asom: $(SRCS) Makefile $(NODE_GENFILES) context.h node.h asom_runtime.h asom_parse.h
	$(CC) $(CFLAGS) -rdynamic -o asom $(SRCS) -ldl -lm

gen $(NODE_GENFILES): node.def asom_gen.rb ../../lib/astrogen.rb
	$(ASTROGEN)

node_specialized.c:
	touch node_specialized.c

CLASSPATH = SOM/Smalltalk:SOM/Examples/Benchmarks:test:.

# Quick smoke test on Examples/Hello.som.
run: asom
	./asom Hello

TESTSUITE_CP = SOM/Smalltalk:SOM/TestSuite:test:.

# Local micro-tests.
test: asom
	./asom Hello
	./asom -cp $(CLASSPATH) Smoke
	./asom -cp $(CLASSPATH) Blocks

# Upstream SOM-st/SOM TestSuite via TestHarness.
testsuite: asom
	@for t in PreliminaryTest BooleanTest StringTest ArrayTest SymbolTest \
		CompilerReturnTest SuperTest ClosureTest ClassLoadingTest \
		SpecialSelectorsTest VectorTest SetTest DictionaryTest \
		HashTest SelfBlockTest GlobalTest CoercionTest; do \
		echo "=== $$t ==="; \
		./asom -cp $(TESTSUITE_CP) TestHarness $$t 2>&1 | tail -5; \
	done

# Run a suite of AreWeFastYet benchmarks (Sieve, Permute, Towers, Queens,
# List, Storage, Bounce, BubbleSort, QuickSort, TreeSort, Mandelbrot).
bench: asom
	./asom -cp $(CLASSPATH) Suite

# AOT-compile a single benchmark and time the cached run.
#   make bench-aot BENCH=Sieve ITERS=200
BENCH ?= Sieve
ITERS ?= 50
bench-aot: asom
	rm -rf code_store
	CCACHE_DISABLE=1 ./asom -c --preload=$(BENCH) -cp $(CLASSPATH) Bench $(BENCH) 1 1 >/dev/null
	./asom -cp $(CLASSPATH) Bench $(BENCH) $(ITERS) 1

bench-pg: asom
	rm -rf code_store
	CCACHE_DISABLE=1 ./asom -p --pg-threshold=1 --preload=$(BENCH) -cp $(CLASSPATH) Bench $(BENCH) 5 1 >/dev/null
	./asom -cp $(CLASSPATH) Bench $(BENCH) $(ITERS) 1

# Side-by-side comparison vs CSOM. Requires CSOM built at $(CSOM_DIR).
# By default, CSOM_DIR=/tmp/csom-ref — set CSOM_DIR=... if you have it
# elsewhere. ITERS defaults to 50 (drop to 5 for a quick smoke check).
ITERS  ?= 50
CSOM_DIR ?= /tmp/csom-ref
compare: asom
	CSOM_DIR=$(CSOM_DIR) ./test/run_compare.sh $(ITERS)

compare-wall: asom
	CSOM_DIR=$(CSOM_DIR) ./test/run_compare.sh --wall $(ITERS)

clean:
	rm -fv asom $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
