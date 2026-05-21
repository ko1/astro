
NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_emit_ast.c \
              node_head.h

RUNTIME=../../runtime

optflags=-O3

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

CALC_SRC_DIR := $(abspath .)
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

calc: main.c parse.c node.c exe_main.c Makefile $(NODE_GENFILES) context.h node.h parse.h \
      $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall $(READLINE_CFLAGS) \
            -DCALC_SRC_DIR='"$(CALC_SRC_DIR)"' \
            -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"' \
            -o calc main.c parse.c node.c -ldl $(READLINE_LDFLAGS)

node_eval.c: ../../lib/* node.def
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

node_specialized.c:
	touch node_specialized.c

clean:
	rm -f calc $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
