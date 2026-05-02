
all: luastro

.PHONY: all clean run test bench

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_hopt.c \
                node_head.h

RUNTIME = ../../runtime
optflags = -O3
CC = gcc

# A note on the build:
#   main.c #includes lua_tokenizer.c, lua_parser.c, lua_runtime.c,
#   lua_stdlib.c so we have only two TUs (main + node).  This mirrors
#   wastro's structure and keeps the gen-file plumbing simple.

SRC = main.c node.c
HDR = context.h node.h
GEN = $(NODE_GENFILES)
LSRC = lua_tokenizer.c lua_parser.c lua_runtime.c lua_stdlib.c

luastro: $(SRC) $(HDR) $(GEN) $(LSRC) Makefile
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function \
	    -rdynamic \
	    -o luastro $(SRC) -ldl -lm -lpthread

node_eval.c: ../../lib/* node.def lua_gen.rb
	ruby -I ../../lib -I . -r astrogen -r lua_gen \
	    -e 'ASTroGen.start([], node_def_class: LuaNodeDef)'

# --- Run a single Lua program ---
run: luastro
	./luastro examples/hello.lua

# --- Tests ---
test: luastro
	@ruby test/run_all.rb

# --- Benchmark: compares plain / -c / -p against lua5.4 / luajit ---
bench: luastro
	@ruby benchmark/run.rb

clean:
	rm -f luastro $(NODE_GENFILES)
	rm -rf code_store
