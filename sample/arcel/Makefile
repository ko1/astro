all: arcel libarcel.a libarcel.so examples/embed

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_head.h

RUNTIME  = ../../runtime
optflags = -O2
CC       = gcc
ASTROGEN = ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

CFLAGS = -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -rdynamic -D_GNU_SOURCE
LIBS   = -ldl -lm

# Library = everything except main.c.  Same translation units, just
# packaged as a static / shared library so embedders link against
# libarcel and skip the CLI scaffolding.
LIB_SRC  = arcel_lib.c node.c value.c input.c parser.c
LIB_OBJ  = $(LIB_SRC:.c=.o)
LIB_HDR  = arcel.h context.h node.h value.h input.h parser.h
LIB_DEPS = $(LIB_OBJ) Makefile $(NODE_GENFILES) $(LIB_HDR)

# CLI binary links against the static library — exact same code paths
# as embedders use.  This keeps "library says X but the CLI does Y"
# from sneaking in.
arcel: main.c libarcel.a Makefile arcel.h
	$(CC) $(CFLAGS) -o arcel main.c libarcel.a $(LIBS)

libarcel.a: $(LIB_DEPS)
	ar rcs $@ $(LIB_OBJ)

libarcel.so: $(LIB_DEPS)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(LIB_SRC) $(LIBS)

%.o: %.c $(NODE_GENFILES) $(LIB_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(NODE_GENFILES): node.def arcel_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./arcel_gen -e 'ASTroGen.start []'

# Embed example: 50-line C program that uses arcel.h to compile and
# evaluate a CEL expression.  Demonstrates the embedding path that
# Envoy / gRPC / custom evaluators would take.
examples/embed: examples/embed.c libarcel.a arcel.h
	$(CC) $(CFLAGS) -o $@ examples/embed.c libarcel.a $(LIBS)

# Run the CEL conformance suite against ./arcel.  Defaults to the
# core / non-proto / non-ext subset; pass --include proto,env,ext etc.
# through TEST_ARGS to widen.
test: arcel
	ruby test/run_conformance.rb $(TEST_ARGS)

# Same suite but routed through the cel-go reference binary —
# validates the harness itself and gives us a per-file ceiling.
test-ref: $(REF)
	ruby test/run_conformance.rb --use-ref $(TEST_ARGS)

# Build the cel-go reference binary used by `test-ref` and benchmark/run.rb.
# Pulls cel-go from proxy.golang.org on first invocation.
celgo:
	$(MAKE) -C test/celgo_ref

# Build the cel-cpp reference binary used by benchmark/run.rb.  Drives an
# out-of-tree cel-cpp checkout (CEL_CPP_DIR; default /tmp/celcpp/cel-cpp)
# via Bazel.  Drop a fresh checkout there with:
#   git clone https://github.com/google/cel-cpp.git /tmp/celcpp/cel-cpp
celcpp:
	$(MAKE) -C test/celcpp_ref

# Benchmark vs cel-go / cel-cpp.  Default 1M iterations per case; pass
# BENCH_ARGS='--iters 5000000 --case predicate' to tune.
bench: arcel celgo
	ruby benchmark/run.rb $(BENCH_ARGS)

# Smoke check that the binary works end-to-end before running the suite.
smoke: arcel
	@./arcel eval -e '1 + 2 * 3'             | grep -q '^7$$' && echo "  ok  arith"
	@./arcel eval -e '(1 + 2) * 3'           | grep -q '^9$$' && echo "  ok  parens"
	@./arcel eval -e '10 % 3'                | grep -q '^1$$' && echo "  ok  modulo"
	@./arcel eval -e '-5 + 10'               | grep -q '^5$$' && echo "  ok  unary minus"
	@./arcel eval -e '7 / 0'                 | grep -q '^ERROR' && echo "  ok  div0 -> error"

# Embed-side smoke: build and run the C example, sanity-check output.
smoke-embed: examples/embed
	@./examples/embed | grep -q '^OK' && echo "  ok  embed example"

clean:
	rm -f arcel libarcel.a libarcel.so examples/embed *.o $(NODE_GENFILES)
	rm -rf code_store

distclean: clean
	$(MAKE) -C test/celgo_ref clean

.PHONY: all test test-ref celgo celcpp bench smoke smoke-embed clean distclean
