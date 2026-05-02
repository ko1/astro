all: astrogre

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
ONIGMO_DIR = ./onigmo
RUNTIME_DIR = ../../runtime

CFLAGS = -I. -I$(RUNTIME_DIR) $(optflags) -ggdb3 -Wall \
         -mavx2 \
         -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
         -Wno-unused-but-set-variable
LDFLAGS =
LIBS = -ldl

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
	rm -fv $(NODE_GENFILES) astrogre selftest_runner

clean-onigmo:
	rm -rf $(ONIGMO_DIR)/build

regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

# Standalone engine self-test / microbench runner.  Doesn't link any
# of main.c's grep CLI code — just the engine + selftest.c.  Used by
# `make self-test`, `make bench`, `make bench-file`.
SELFTEST_SRCS = selftest_runner.c selftest.c node.c parse.c match.c \
                backend_astrogre.c aho_corasick.c

selftest_runner: $(SELFTEST_SRCS) $(NODE_GENFILES) Makefile node.h context.h \
                 backend.h aho_corasick.h $(ASTROGRE_DEPS)
	$(CC) $(CFLAGS) -o $@ $(SELFTEST_SRCS) $(LDFLAGS) $(LIBS)

self-test: selftest_runner
	./selftest_runner test

# Engine microbench (per-pattern ns/match — internal numbers).
bench: selftest_runner
	./selftest_runner bench

# In-engine bench on a real input file.  Pattern uses /pat/flags
# syntax (same as `--dump`).  Example:
#   make bench-file FILE=bench/corpus_big.txt PATTERN=/static/ ITERS=50
FILE    ?= bench/corpus_big.txt
PATTERN ?= /static/
ITERS   ?= 50
bench-file: selftest_runner
	./selftest_runner bench-file $(FILE) $(PATTERN) $(ITERS)

# Build-bench helpers.  `are` is the production CLI; benches that
# compare against grep / rg need it built.
are/are: are/main.c are/types.c are/ignore.c are/work.c \
         $(SRCS) $(NODE_GENFILES) aho_corasick.h are/types.h are/ignore.h are/work.h
	$(MAKE) -C are

# All bench harnesses are Ruby scripts so the timing logic, env
# var handling, and TSV emit live in one readable place.
# Pass N=<runs> on the make line to override best-of-3.
N ?= 3

# Per-pattern engine comparison on a single 118 MB corpus.
# Each pattern lights up grep / ripgrep / astrogre {plain, +onigmo,
# aot/first, aot/cached} / are -j1 in turn.  RAW=path/to.tsv adds
# per-run TSV alongside the human summary.
bench-rg: astrogre are/are
	cd bench && ruby grep_bench.rb $(N)

# Recursive tree walk: are -j4 vs ripgrep vs grep -r on /usr/include
# and the astro tree.  Exercises the .gitignore-aware walker.
bench-tree: astrogre are/are
	cd bench && ruby tree_bench.rb $(N)

# In-engine `--bench-file` harness on AOT-favourable patterns —
# captures the AOT specialiser's effect without grep CLI overhead.
bench-aot: astrogre
	cd bench && ruby aot_bench.rb $(N)

# Run everything in one go.
bench-all: bench bench-rg bench-tree bench-aot

.PHONY: all clean clean-onigmo regen self-test \
        bench bench-file bench-rg bench-tree bench-aot bench-all
