all: astr

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
ASTROGEN=ruby -I ../../lib -r ./astr_gen -e 'ASTroGen.start([], node_def_class: AstrNodeDef)'

astr: main.c node.c parse.c runtime.c Makefile $(NODE_GENFILES) context.h node.h
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -rdynamic \
	    -o astr main.c node.c parse.c runtime.c -ldl -lm -lgc

$(NODE_GENFILES): node.def astr_gen.rb ../../lib/astrogen.rb
	$(ASTROGEN)

run: astr
	./astr test.r

c: astr
	./astr -c test.r

test: astr
	ruby test/run_tests.rb

bench: astr
	ruby benchmark/bench.rb

clean:
	rm -f astr $(NODE_GENFILES) *.o
	rm -rf code_store

.PHONY: all run c test bench clean
