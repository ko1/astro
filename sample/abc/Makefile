
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

optflags=-O2

# bc keeps numbers arbitrary-precision (GMP) and values are GC-managed (libgc).
LIBS=-lgmp -lgc -ldl

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

ABC_SRC_DIR := $(abspath .)
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

abc: main.c parse.c node.c bcnum.c exe_main.c Makefile $(NODE_GENFILES) \
     context.h node.h parse.h bcnum.h \
     $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wextra -Wno-unused-parameter -rdynamic $(READLINE_CFLAGS) \
	    -DABC_SRC_DIR='"$(ABC_SRC_DIR)"' \
	    -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"' \
	    -o abc main.c parse.c node.c bcnum.c $(LIBS) $(READLINE_LDFLAGS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

check: abc
	ruby test/run_tests.rb

bench: abc
	ruby benchmark/run_bench.rb

clean:
	rm -f abc $(NODE_GENFILES)
	rm -rf code_store
