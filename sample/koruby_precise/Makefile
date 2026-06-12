
all: koruby_precise

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
	rm -fv $(NODE_GENFILES) koruby_precise
	rm -rf code_store

clean: clean_bench

KORUBY_OPT=--plain

run: koruby_precise
	time -p ./koruby_precise $(KORUBY_OPT) test.ba.rb

gdb: koruby_precise
	gdb -ex run --args ./koruby_precise $(KORUBY_OPT) test.ba.rb

# GC testbed bench suite — same as koruby for comparison.

# Tests: current binary で全 T_*.ba.rb を実行、 test/expected/*.expected と
# 比較。 expected が無ければ初回実行時に生成する (= oracle generation mode)。
#
# 派生 target:
#   make test                    現 binary で全 T_*.ba.rb
#   make test_all                全 16 backend で rebuild + test
#   make test_stress             KORUBY_GC_STRESS=1 で全 T_*.ba.rb
#   make test_all_stress         16 backend × stress mode

test_stress: koruby_precise
	ruby test/run.rb --stress

test_all:
	ruby test/run.rb --all

test_all_stress:
	ruby test/run.rb --all --stress

# ASTRO_DEBUG: enables ASTRO_ASSERT + stress-mode GC invariant checks.
# Iter 35 fairness fix: the default flipped to 0 (release / perf-friendly).
# Past perf measurements were taken with ASTRO_DEBUG=1 implicit-from-context.h
# which is a build-flag bias the user objected to.  Measured impact on
# binary_trees was <1% (most asserts constant-fold under -O3) but the
# *principle* — perf builds shouldn't include assert overhead — was correct.
# Override with `make ASTRO_DEBUG=1` for development / bug hunts.
ASTRO_DEBUG ?= 0

# GC backend selector.  Default `copy` (= Cheney semi-space).  See the
# GC_NUM_<name> table below for the full list of supported backends.
# Build with: make GC=<name>  (e.g. make GC=mark)
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
# iter 74: precise GC framework は runtime/precise_gc/ 配下に移動。
# sample 側 (context.h) が AROH_SCAN_EDGES macro + AroGcKind 拡張値を
# 提供して contract が成立する。
PRECISE_GC_DIR := $(RUNTIME)/precise_gc
GC_SRC := $(PRECISE_GC_DIR)/gc_$(GC).c

# gc_common.c hosts framework helpers shared by all backends:
#   - aro_gc_realloc_in_place (weak default; backends override)
#   - aro_gc_reset_payload_header
# (iter 76: aro_gc_realloc_payload は sample 側に移動)
# gc_none も含めて全 backend で必要なので unconditional に link。
GC_COMMON_SRC := $(PRECISE_GC_DIR)/gc_common.c

ifeq ($(GC_NUM),)
$(error Unknown GC backend "$(GC)" — pick one of: none mark mark_gen mark_gen_inc copy copy_gen mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap_gen mark_card_gen mark_freelist)
endif

# Track current GC selection so a `make GC=foo` after `make GC=bar` actually
# rebuilds, even when no .c has been touched.  Without this the binary
# silently kept the previous backend.  The marker file's content is the GC
# name (+ "-audit" suffix when ARO_GC_WB_AUDIT=1); we rewrite it (touching
# mtime) whenever it differs from the current selection so toggling the
# audit flag also forces a rebuild.
GC_VARIANT := $(GC)$(if $(ARO_GC_WB_AUDIT),-audit)
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC_VARIANT))
$(shell echo "$(GC_VARIANT)" > .built_gc)
endif

# Absolute paths embedded into the binary so AOT-bake compilations
# (run in code_store/) can find runtime + sample headers.  See main.c
# common_build_flags_and_link.
KORUBY_PRECISE_DIR := $(abspath .)
RUNTIME_ABSPATH    := $(abspath $(RUNTIME))
PRISM_INC_ABSPATH  := $(abspath ./prism/include)

koruby_precise: .built_gc *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c $(GC_COMMON_SRC) $(GC_SRC) $(PRECISE_GC_DIR)/gc.h $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h $(RUNTIME)/astro_debug.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -flto=auto -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            -DKORUBY_GC=$(GC_NUM) \
            -DKORUBY_PRECISE_DIR='"$(KORUBY_PRECISE_DIR)"' \
            -DASTRO_RUNTIME_DIR='"$(RUNTIME_ABSPATH)"' \
            -DASTRO_PRISM_INC_DIR='"$(PRISM_INC_ABSPATH)"' \
            $(if $(ASTRO_DEBUG),-DASTRO_DEBUG=$(ASTRO_DEBUG)) \
            $(if $(ARO_GC_WB_AUDIT),-DARO_GC_WB_AUDIT -Werror=discarded-qualifiers -Werror=cast-qual) \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o koruby_precise main.c node.c node_slowpath.c parse.c astro_jit.c $(GC_COMMON_SRC) $(GC_SRC) -lprism -ldl

$(NODE_GENFILES) node_head.h &: ../../lib/* node.def koruby_gen.rb
	ruby -I ../../lib -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KoRubyNodeDef)'


# ---------------------------------------------------------------------------
# rubyharness — shared CRuby-oracle diff tests + multi-mode bench.
#   make gen / make test [CAT=...] [STRESS=1] / make bench
# ---------------------------------------------------------------------------
INTERP ?= ./koruby_precise
include ../rubyharness/harness.mk
test bench: koruby_precise
