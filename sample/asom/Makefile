
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

ITERS ?= 1
BENCH ?= Sieve

# Cross-engine AreWeFastYet comparison: asom (interp/aot/pg) vs SOM++,
# TruffleSOM. Prints two tables — inner-work (engine `system ticks`,
# excludes startup) and wall-clock (full process). Each cell is
# best-of-3. INNER iterations per bench are tuned in run_compare.rb so
# one outer fills ~1 s on asom-interp; ITERS multiplies that for longer
# runs (default 1 ⇒ sustained ~1 s, ITERS=5 ⇒ ~5 s per cell).
# Override engine paths via SOMPP_DIR / TRUFFLESOM_DIR / MX_DIR
# (defaults assume /tmp/<name>-ref).
bench: asom
	ruby ./test/run_compare.rb $(ITERS)

# Local smoke run of the asom-only Suite.som (exercises a subset of
# AreWeFastYet bench classes for verifyResult coverage, no engine
# comparison, no timing). Useful as a quick "does it still run" check.
bench-suite: asom
	./asom -cp $(CLASSPATH) Suite

# AOT-compile a single benchmark and time the cached run.
#   make bench-aot BENCH=Sieve ITERS=200
bench-aot: asom
	rm -rf code_store
	CCACHE_DISABLE=1 ./asom -c --preload=$(BENCH) -cp $(CLASSPATH) Bench $(BENCH) 1 1 >/dev/null
	./asom -cp $(CLASSPATH) Bench $(BENCH) $(ITERS) 1

bench-pg: asom
	rm -rf code_store
	CCACHE_DISABLE=1 ./asom -p --pg-threshold=1 --preload=$(BENCH) -cp $(CLASSPATH) Bench $(BENCH) 5 1 >/dev/null
	./asom -cp $(CLASSPATH) Bench $(BENCH) $(ITERS) 1

clean:
	rm -fv asom $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
