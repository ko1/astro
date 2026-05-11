all: arawk

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
CC=gcc
ASTROGEN=ruby -I ../../lib -r ./awk_gen -e 'ASTroGen.start([], node_def_class: AwkNodeDef)'

arawk: main.c node.c parse.c runtime.c Makefile $(NODE_GENFILES) context.h node.h
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -rdynamic \
	    -o arawk main.c node.c parse.c runtime.c -ldl -lm -lgc

$(NODE_GENFILES): node.def awk_gen.rb ../../lib/astrogen.rb
	$(ASTROGEN)

run: arawk
	./arawk -e 'BEGIN { print 1 + 2 }'

test: arawk
	ruby test/run_tests.rb
	ruby test/run_bench_tests.rb

bench: arawk
	ruby benchmark/bench.rb

clean:
	rm -f arawk $(NODE_GENFILES) *.o
	rm -rf code_store

.PHONY: all run test clean
