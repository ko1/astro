all: aforth

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
RUNTIME = ../../runtime
ASTRO_LIB = ../../lib

CFLAGS  = -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall \
          -Wno-unused-function -Wno-unused-variable \
          -Wno-unused-parameter -Wno-unused-but-set-variable

aforth: main.c parse.c node.c $(NODE_GENFILES) Makefile context.h node.h parse.h \
        $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h
	$(CC) $(CFLAGS) -rdynamic -o $@ main.c parse.c node.c -ldl

$(NODE_GENFILES): node.def $(ASTRO_LIB)/astrogen.rb
	ruby -I $(ASTRO_LIB) -r astrogen -e 'ASTroGen.start []'

clean:
	rm -fv aforth $(NODE_GENFILES)
	rm -rf code_store

regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

# Quick smoke tests
test: aforth
	@bash -c 'set -e; for f in test/test_*.fs; do \
	    echo "=== $$f ==="; \
	    ./aforth -q "$$f" || echo "  FAILED"; \
	done'

# Benchmarks (interpreter only by default; pass mode=aot to use code_store)
bench: aforth
	ruby benchmark/run.rb

# AOT compile every entry: run once, then code_store/all.so contains all SDs.
aot-build: aforth
	rm -rf code_store
	@for f in benchmark/bm_*.fs test/test_*.fs; do \
	    [ -f "$$f" ] && ./aforth --aot-compile "$$f" >/dev/null 2>&1 || true; \
	done
	@ls -la code_store/all.so 2>/dev/null || echo "no all.so built"

.PHONY: all clean regen test bench aot-build
