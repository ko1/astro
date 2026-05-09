
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

# Fetch upstream PolyBench/C 4.2.1 source (one-time setup) into
# benchmark/polybench/upstream/ for reference / future kernel adapt
# work.  The adapted kernels in benchmark/polybench/kernels/ ship in-tree
# and don't need this fetch — but the upstream source is what they
# track, and having it locally lets you compare line-for-line.
benchmark/polybench/upstream/.fetched:
	bash benchmark/polybench/fetch.sh
	touch benchmark/polybench/upstream/.fetched

# Run all adapted PolyBench kernels under castro AOT vs gcc -O0/-O3.
# Outputs a median-of-RUNS table (default RUNS=7).
bench-polybench: castro
	bash benchmark/polybench/run.sh

# Computer Language Benchmarks Game — non-numeric kernels (binary-trees,
# fannkuch-redux, md5, nbody).  Probes the malloc / control-flow /
# bit-ops / FP-simulation corners that polybench doesn't touch.
bench-clbg: castro
	bash benchmark/clbg/run.sh

.PHONY: clean test test-cts bench bench-polybench bench-clbg
