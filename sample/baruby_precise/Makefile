
all: baruby_precise

.PHONY: crun bench clean clean_bench

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_hopt.c \
              node_head.h

RUNTIME=../../runtime

optflags=-O3
BITEM=fib
ITEM=test.ba.rb
CC=gcc

clean_bench:
	rm -fv $(NODE_GENFILES) baruby_precise
	rm -rf code_store

clean: clean_bench

BARUBY_OPT=--plain

run: baruby_precise
	time -p ./baruby_precise $(BARUBY_OPT) test.ba.rb

gdb: baruby_precise
	gdb -ex run --args ./baruby_precise $(BARUBY_OPT) test.ba.rb

# GC testbed bench suite — same as baruby for comparison.
bench: baruby_precise
	BARUBY_BIN=./baruby_precise ruby bench/run.rb

# ASTRO_DEBUG=1 (default in context.h) enables ASTRO_ASSERT and the
# stress-mode GC invariant checks.  Override on the command line with
# `make ASTRO_DEBUG=0` for a release-shape build.
ASTRO_DEBUG ?=

# GC backend selector.  Default `copy` (= Cheney semi-space).  Available:
#   none, mark, mark_gen, mark_gen_inc, copy, copy_gen, copy_gen_inc
# Build with: make GC=mark
GC ?= copy
GC_NUM_none             := 1
GC_NUM_mark             := 2
GC_NUM_mark_gen         := 3
GC_NUM_mark_gen_inc     := 4
GC_NUM_copy             := 5
GC_NUM_copy_gen         := 6
GC_NUM_copy_gen_inc     := 7
GC_NUM_mark_compact     := 8
GC_NUM_mark_compact_gen := 9
GC_NUM_bump             := 10
GC_NUM_mark_bump_gen    := 11
GC_NUM_immix            := 12
GC_NUM_immix_gen        := 13
GC_NUM_mark_bitmap_gen      := 14
GC_NUM := $(GC_NUM_$(GC))
GC_SRC := gc_$(GC).c

ifeq ($(GC_NUM),)
$(error Unknown GC backend "$(GC)" — pick one of: none mark mark_gen mark_gen_inc copy copy_gen copy_gen_inc mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap_gen)
endif

# Track current GC selection so a `make GC=foo` after `make GC=bar` actually
# rebuilds, even when no .c has been touched.  Without this the binary
# silently kept the previous backend.  The marker file's content is the GC
# name; we rewrite it (touching mtime) whenever it differs from $(GC).
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC))
$(shell echo "$(GC)" > .built_gc)
endif

baruby_precise: .built_gc *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c $(GC_SRC) gc.h $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h $(RUNTIME)/astro_debug.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -flto=auto -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            -DBARUBY_GC=$(GC_NUM) \
            $(if $(ASTRO_DEBUG),-DASTRO_DEBUG=$(ASTRO_DEBUG)) \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o baruby_precise main.c node.c node_slowpath.c baruby_parse.c astro_jit.c $(GC_SRC) -lprism -ldl

node_eval.c: ../../lib/* node.def baruby_gen.rb
	ruby -I ../../lib -r ./baruby_gen -e 'ASTroGen.start([], node_def_class: BaRubyNodeDef)'

