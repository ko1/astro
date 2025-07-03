
NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_specialized.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

optflags=-O3

calc: *.c Makefile $(NODE_GENFILES) node.c
	gcc -I. $(optflags) -ggdb3 -Wall \
            -o calc main.c node.c

node_eval.c: ../../lib/* node.def
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

node_specialized.c:
	touch node_specialized.c

