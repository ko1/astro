
NODE_GENFILES=node_eval.c node_dispatch.c node_dump.c node_specialize.c \
              node_replace.c node_alloc.c node_hash.c node_emit_ast.c node_head.h

RUNTIME=../../runtime
optflags=-O2
LIBS=-lgc -ldl

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

ANPY_SRC_DIR := $(abspath .)
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

SRCS=main.c lexer.c parse.c check.c node.c value.c

anpy: $(SRCS) exe_main.c Makefile $(NODE_GENFILES) \
      context.h node.h parse.h check.h value.h \
      $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wextra -Wno-unused-parameter -rdynamic $(READLINE_CFLAGS) \
	    -DANPY_SRC_DIR='"$(ANPY_SRC_DIR)"' -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"' \
	    -o anpy $(SRCS) $(LIBS) $(READLINE_LDFLAGS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

check: anpy
	ruby test/run_tests.rb

bench: anpy
	ruby benchmark/run_bench.rb

clean:
	rm -f anpy $(NODE_GENFILES)
	rm -rf code_store
