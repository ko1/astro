all: astrogre

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
PRISM_DIR = ./prism
ONIGMO_DIR = ./onigmo
RUNTIME_DIR = ../../runtime

CFLAGS = -I$(PRISM_DIR)/include -I. -I$(RUNTIME_DIR) $(optflags) -ggdb3 -Wall \
         -mavx2 \
         -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
         -Wno-unused-but-set-variable
LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(CURDIR)/$(PRISM_DIR)/build
LIBS = -lprism -ldl

SRCS = main.c node.c parse.c match.c selftest.c backend_astrogre.c aho_corasick.c

# Onigmo backend (built locally; opt-in via WITH_ONIGMO=1)
ifdef WITH_ONIGMO
CFLAGS += -I$(ONIGMO_DIR) -I$(ONIGMO_DIR)/enc/unicode -DUSE_ONIGMO
SRCS += backend_onigmo.c
LIBS += $(ONIGMO_DIR)/build/libonigmo.a
ASTROGRE_DEPS = $(ONIGMO_DIR)/build/libonigmo.a
endif

astrogre: $(SRCS) $(NODE_GENFILES) Makefile node.h context.h backend.h aho_corasick.h $(ASTROGRE_DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

$(ONIGMO_DIR)/build/libonigmo.a: $(ONIGMO_DIR)/build_local.mk
	$(MAKE) -C $(ONIGMO_DIR) -f build_local.mk

clean:
	rm -fv $(NODE_GENFILES) astrogre

clean-onigmo:
	rm -rf $(ONIGMO_DIR)/build

regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

self-test: astrogre
	./astrogre --self-test

# Engine microbench (per-pattern ns/match — internal numbers).
bench: astrogre
	./astrogre --bench

# Build-bench helpers.  `are` is the production CLI; benches that
# compare against grep / rg need it built.
are/are: are/main.c are/types.c are/ignore.c are/work.c \
         $(SRCS) $(NODE_GENFILES) aho_corasick.h are/types.h are/ignore.h are/work.h
	$(MAKE) -C are

# CLI vs grep / ripgrep on a single 118 MB corpus.
bench-rg: astrogre are/are
	./bench/grep_bench.sh

# CLI vs grep / ripgrep on recursive tree walks (.gitignore-aware).
bench-tree: astrogre are/are
	./bench/tree_bench.sh

# Engine variants (interp / aot / +onigmo) vs grep / rg on the same
# corpus — for measuring the AOT specialiser's effect.
bench-aot: astrogre
	./bench/aot_bench.sh

# Run everything in one go.
bench-all: bench bench-rg bench-tree bench-aot

.PHONY: all clean clean-onigmo regen self-test \
        bench bench-rg bench-tree bench-aot bench-all
