
NODE_GENFILES=node_eval.c node_dispatch.c node_dump.c node_specialize.c \
              node_replace.c node_alloc.c node_hash.c node_emit_ast.c node_head.h

RUNTIME=../../runtime
optflags=-O2
LIBS=-lgc -ldl -lm

ANLOX_SRC_DIR := $(abspath .)
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

SRCS=main.c lexer.c parse.c node.c value.c

anlox: $(SRCS) Makefile $(NODE_GENFILES) \
       context.h node.h parse.h \
       $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wextra -Wno-unused-parameter -rdynamic \
	    -DANLOX_SRC_DIR='"$(ANLOX_SRC_DIR)"' -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"' \
	    -o anlox $(SRCS) $(LIBS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

check: anlox
	ruby test/run_tests.rb

bench: anlox
	ruby benchmark/run_bench.rb

clean:
	rm -f anlox $(NODE_GENFILES)
	rm -rf code_store
