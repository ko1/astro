
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

castro: main.c node.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -rdynamic \
	    -o castro main.c node.c -ldl

node_eval.c: ../../lib/* node.def castro_gen.rb
	ruby -I ../../lib -r ./castro_gen -e 'ASTroGen.start([], node_def_class: CastroNodeDef)'

node_specialized.c:
	touch node_specialized.c

clean:
	rm -f castro $(NODE_GENFILES) node_specialized.c
	rm -rf code_store

# Run feature tests (tests/test_*.c) under interp / AOT first / AOT cached
# modes, comparing each output against gcc -O0.
test: castro
	bash tests/run.sh

# Run the c-testsuite (must be present under testsuite/c-testsuite/single-exec).
test-cts: castro
	bash run_tests.sh

# Run the benchmark suite vs gcc -O0..-O3.
bench: castro
	ruby bench.rb

.PHONY: clean test test-cts bench
