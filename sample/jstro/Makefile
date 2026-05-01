all: jstro

.PHONY: all clean run test bench compiled_jstro pg_jstro

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

# main.c includes js_runtime.c / js_parser.c / js_stdlib.c so the build
# is two TUs (main + node).  node.c #includes astro_node.c +
# astro_code_store.c + node_specialized.c.
SRC = main.c node.c
HDR = context.h node.h
GEN = $(NODE_GENFILES)
LSRC = js_runtime.c js_parser.c js_stdlib.c

jstro: $(SRC) $(HDR) $(GEN) $(LSRC) node_specialized.c Makefile
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function \
	    -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
	    -rdynamic \
	    -o jstro $(SRC) $(LSRC) -ldl -lm -lpthread

node_eval.c: ../../lib/* node.def jstro_gen.rb
	ruby -I ../../lib -I . -r astrogen -r jstro_gen \
	    -e 'ASTroGen.start([], node_def_class: JstroNodeDef)'

# Placeholder; -c / --aot-compile / -p overwrite with real bake output.
node_specialized.c:
	@echo "// placeholder — overwritten by ./jstro -c / -p" > $@

# --- Run a single JS program ---
run: jstro
	./jstro examples/hello.js

ITEM ?= examples/hello.js

# --- AOT (compiled_jstro): -c run + recompile with the baked SD blob ---
compiled_jstro: jstro
	rm -f node_specialized.c
	./jstro -c $(ITEM)
	cp node_specialized.c compiled_node_specialized.c
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function \
	    -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
	    -rdynamic \
	    -o compiled_jstro $(SRC) $(LSRC) -ldl -lm -lpthread

# --- Profile-guided (pg_jstro): -p run then bake observed SDs ---
pg_jstro: jstro
	rm -f node_specialized.c
	./jstro -p $(ITEM)
	cp node_specialized.c pg_node_specialized.c
	$(CC) -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function \
	    -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
	    --param=early-inlining-insns=200 \
	    -rdynamic \
	    -o pg_jstro $(SRC) $(LSRC) -ldl -lm -lpthread

# --- Tests ---
test: jstro
	@ruby test/run_all.rb

# --- Benchmark ---
bench: jstro
	@ruby benchmark/run.rb

clean:
	rm -f jstro compiled_jstro pg_jstro $(NODE_GENFILES) \
	      node_specialized.c compiled_node_specialized.c pg_node_specialized.c
	rm -rf code_store
