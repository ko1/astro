
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

wastro: main.c node.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall \
	    -rdynamic \
	    -o wastro main.c node.c -ldl -lm

node_eval.c: ../../lib/* node.def
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

node_specialized.c:
	touch node_specialized.c

run: wastro
	./wastro examples/fib.wat fib 10
	./wastro examples/fib.wat fib 30

clean:
	rm -f wastro $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
