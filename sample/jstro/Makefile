all: jstro

.PHONY: all clean run test bench

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_head.h

RUNTIME = ../../runtime
optflags = -O3 -fno-stack-protector -fno-stack-clash-protection -fno-plt -fno-semantic-interposition
CC = gcc

SRC = main.c node.c
HDR = context.h node.h
GEN = $(NODE_GENFILES)
LSRC = js_runtime.c js_parser.c js_stdlib.c

jstro: $(SRC) $(HDR) $(GEN) $(LSRC) Makefile
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function \
	    -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
	    -rdynamic \
	    -o jstro $(SRC) $(LSRC) -ldl -lm

node_eval.c: ../../lib/* node.def jstro_gen.rb
	ruby -I ../../lib -I . -r astrogen -r jstro_gen \
	    -e 'ASTroGen.start([], node_def_class: JstroNodeDef)'

# --- Run a single JS program ---
run: jstro
	./jstro examples/hello.js

ITEM ?= examples/hello.js

# --- Tests ---
test: jstro
	@ruby test/run_all.rb

# --- Benchmark ---
bench: jstro
	@ruby benchmark/run.rb

clean:
	rm -f jstro $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
