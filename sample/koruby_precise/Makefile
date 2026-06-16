# koruby_precise v2 — build (docs/v2_spec.md §5).
#
# Sources are explicitly listed (no *.c glob) and the GC backend selection
# is tracked through a marker file so `make GC=foo` after `make GC=bar`
# relinks even when no source changed.

all: koruby_precise

.PHONY: all run clean

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_emit_ast.c \
                node_head.h

RUNTIME = ../../runtime
LIB     = ../../lib

CC ?= gcc
optflags = -O2

# ASTRO_DEBUG: ASTRO_ASSERT + invariant checks.  Off by default (perf-true
# builds); enable with `make ASTRO_DEBUG=1` for bug hunts.
ASTRO_DEBUG ?= 0

# GC backend selector — default copy (moving) from day one (v2_design §9.1).
GC ?= copy
GC_NUM_none             := 1
GC_NUM_mark             := 2
GC_NUM_mark_gen         := 3
GC_NUM_mark_gen_inc     := 4
GC_NUM_copy             := 5
GC_NUM_copy_gen         := 6
GC_NUM_mark_compact     := 8
GC_NUM_mark_compact_gen := 9
GC_NUM_bump             := 10
GC_NUM_mark_bump_gen    := 11
GC_NUM_immix            := 12
GC_NUM_immix_gen        := 13
GC_NUM_mark_bitmap_gen  := 14
GC_NUM_mark_card_gen    := 15
GC_NUM_mark_freelist    := 16
GC_NUM := $(GC_NUM_$(GC))
ifeq ($(GC_NUM),)
$(error Unknown GC backend "$(GC)")
endif

PRECISE_GC_DIR := $(RUNTIME)/precise_gc
GC_SRC         := $(PRECISE_GC_DIR)/gc_$(GC).c
GC_COMMON_SRC  := $(PRECISE_GC_DIR)/gc_common.c

# Marker file: rebuild when the GC selection (or debug level) changes.
GC_VARIANT := $(GC)-d$(ASTRO_DEBUG)
GC_MARKER  := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC_VARIANT))
$(shell echo "$(GC_VARIANT)" > .built_gc)
endif

KORUBY_SRC_DIR     := $(abspath .)
RUNTIME_ABSPATH    := $(abspath $(RUNTIME))
PRISM_INC_ABSPATH  := $(abspath ./prism/include)

SRCS = main.c node.c korb_runtime.c parse.c
# builtins/*.c are #included into korb_runtime.c's TU (not compiled separately);
# list them + korb_runtime.h so edits trigger a rebuild.
BUILTINS_INCLUDED = $(wildcard builtins/*.c)

koruby_precise: .built_gc Makefile $(SRCS) context.h node.h korb_runtime.h $(BUILTINS_INCLUDED) $(NODE_GENFILES) \
                $(GC_COMMON_SRC) $(GC_SRC) $(PRECISE_GC_DIR)/gc.h $(PRECISE_GC_DIR)/gc_types.h \
                $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h \
                $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h $(RUNTIME)/astro_ref_template.h
	$(CC) -I. -I./prism/include -I$(RUNTIME) $(optflags) -flto=auto -ggdb3 -Wall -Wextra \
	    -Wno-unused-parameter -rdynamic -fno-plt \
	    -DBARUBY_GC=$(GC_NUM) \
	    -DASTRO_DEBUG=$(ASTRO_DEBUG) \
	    -DKORUBY_SRC_DIR='"$(KORUBY_SRC_DIR)"' \
	    -DASTRO_RUNTIME_DIR='"$(RUNTIME_ABSPATH)"' \
	    -DASTRO_PRISM_INC_DIR='"$(PRISM_INC_ABSPATH)"' \
	    -o koruby_precise $(SRCS) $(GC_COMMON_SRC) $(GC_SRC) ./prism/build/libprism.a -ldl -lm

# Generated files: grouped target (single generator invocation).
$(NODE_GENFILES) &: node.def koruby_gen.rb $(LIB)/astrogen.rb
	ruby -I $(LIB) -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KorubyNodeDef)'

run: koruby_precise
	./koruby_precise -e 'def fib(n) = n < 2 ? n : fib(n-1) + fib(n-2); puts fib(25)'

clean:
	rm -f koruby_precise $(NODE_GENFILES) .built_gc
	rm -rf code_store

# rubyharness (shared differential tests + bench)
INTERP ?= ./koruby_precise
include ../rubyharness/harness.mk
test bench: koruby_precise
