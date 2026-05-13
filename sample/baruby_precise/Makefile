
all: baruby_precise

.PHONY: crun bench clean clean_bench

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_hopt.c \
              node_head.h

RUNTIME=../../runtime

optflags=-O3
BITEM=fib
ITEM=test.ba.rb
CC=gcc

clean_bench:
	rm -fv $(NODE_GENFILES) baruby_precise
	rm -rf code_store

clean: clean_bench

BARUBY_OPT=--plain

run: baruby_precise
	time -p ./baruby_precise $(BARUBY_OPT) test.ba.rb

gdb: baruby_precise
	gdb -ex run --args ./baruby_precise $(BARUBY_OPT) test.ba.rb

# GC testbed bench suite — same as baruby for comparison.
bench: baruby_precise
	BARUBY_BIN=./baruby_precise ruby bench/run.rb

# ASTRO_DEBUG=1 (default in context.h) enables ASTRO_ASSERT and the
# stress-mode GC invariant checks.  Override on the command line with
# `make ASTRO_DEBUG=0` for a release-shape build.
ASTRO_DEBUG ?=

baruby_precise: *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c gc.c $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h $(RUNTIME)/astro_debug.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            $(if $(ASTRO_DEBUG),-DASTRO_DEBUG=$(ASTRO_DEBUG)) \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o baruby_precise main.c node.c node_slowpath.c baruby_parse.c astro_jit.c gc.c -lprism -ldl

node_eval.c: ../../lib/* node.def baruby_gen.rb
	ruby -I ../../lib -r ./baruby_gen -e 'ASTroGen.start([], node_def_class: BaRubyNodeDef)'

