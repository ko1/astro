
all: baruby

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
	rm -fv $(NODE_GENFILES) baruby b0 b1 b2 b3 b_spinel
	rm -rf code_store

clean: clean_bench

BARUBY_OPT=

run: baruby
	time -p ./baruby $(BARUBY_OPT) test.ba.rb

runb:
	./baruby -b test.ba.rb

crun:
	make clean
	make run

ptrain:
	make clean
	make baruby
	./baruby $(ITEM) -p
	rm -f baruby
	make baruby

ctrain:
	make clean
	make baruby
	./baruby -c $(ITEM)
	rm -f baruby
	make baruby

gdb: baruby
	gdb -ex run --args ./baruby $(BARUBY_OPT) test.ba.rb

c: baruby
	./baruby -c test.ba.rb

# GC testbed bench suite.  Runs every bench/*.ba.rb under ./baruby and
# reports wall time + libgc counters.  See bench/run.rb for options
# (e.g. `--mode aot` once code_store is built, `-n 5` for more reps).
bench: baruby
	ruby bench/run.rb

baruby: *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o baruby main.c node.c node_slowpath.c baruby_parse.c astro_jit.c -lprism -lgc -ldl

node_eval.c: ../../lib/* node.def baruby_gen.rb
	ruby -I ../../lib -r ./baruby_gen -e 'ASTroGen.start([], node_def_class: BaRubyNodeDef)'

