
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
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall \
	    -o castro main.c node.c -ldl

node_eval.c: ../../lib/* node.def castro_gen.rb
	ruby -I ../../lib -r ./castro_gen -e 'ASTroGen.start([], node_def_class: CastroNodeDef)'

node_specialized.c:
	touch node_specialized.c

clean:
	rm -f castro $(NODE_GENFILES) node_specialized.c
	rm -rf code_store

.PHONY: clean
