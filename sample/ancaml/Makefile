
NODE_GENFILES=node_eval.c node_dispatch.c node_dump.c node_specialize.c \
              node_replace.c node_alloc.c node_hash.c node_emit_ast.c node_head.h

RUNTIME=../../runtime
optflags=-O2
LIBS=-lgc -ldl -lm

ANCAML_SRC_DIR := $(abspath .)
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

SRCS=main.c lexer.c parse.c type.c node.c value.c

ancaml: $(SRCS) exe_main.c Makefile $(NODE_GENFILES) \
      context.h node.h parse.h type.h \
      $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wextra -Wno-unused-parameter -rdynamic \
	    -DANCAML_SRC_DIR='"$(ANCAML_SRC_DIR)"' -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"' \
	    -o ancaml $(SRCS) $(LIBS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

check: ancaml
	ruby test/run_tests.rb

bench: ancaml
	ruby benchmark/run_bench.rb

clean:
	rm -f ancaml $(NODE_GENFILES)
	rm -rf code_store
