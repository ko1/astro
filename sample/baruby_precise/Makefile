
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

# Tests: current binary で全 T_*.ba.rb を実行、 test/expected/*.expected と
# 比較。 expected が無ければ初回実行時に生成する (= oracle generation mode)。
#
# 派生 target:
#   make test                    現 binary で全 T_*.ba.rb
#   make test_all                全 16 backend で rebuild + test
#   make test_stress             BARUBY_GC_STRESS=1 で全 T_*.ba.rb
#   make test_all_stress         16 backend × stress mode
test: baruby_precise
	ruby test/run.rb

test_stress: baruby_precise
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
GC_NUM_mark_bitmap_gen  := 14
GC_NUM_mark_card_gen    := 15
GC_NUM_mark_freelist    := 16
GC_NUM_copy_scramble    := 17
GC_NUM := $(GC_NUM_$(GC))
# iter 74: precise GC framework は runtime/precise_gc/ 配下に移動。
# sample 側 (context.h) が ASTRO_GC_SCAN_EDGES macro + AroGcKind 拡張値を
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
$(error Unknown GC backend "$(GC)" — pick one of: none mark mark_gen mark_gen_inc copy copy_gen copy_gen_inc mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap_gen mark_card_gen mark_freelist copy_scramble)
endif

# Track current GC selection so a `make GC=foo` after `make GC=bar` actually
# rebuilds, even when no .c has been touched.  Without this the binary
# silently kept the previous backend.  The marker file's content is the GC
# name; we rewrite it (touching mtime) whenever it differs from $(GC).
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC))
$(shell echo "$(GC)" > .built_gc)
endif

# Absolute paths embedded into the binary so AOT-bake compilations
# (run in code_store/) can find runtime + sample headers.  See main.c
# common_build_flags_and_link.
BARUBY_PRECISE_DIR := $(abspath .)
RUNTIME_ABSPATH    := $(abspath $(RUNTIME))
PRISM_INC_ABSPATH  := $(abspath ./prism/include)

baruby_precise: .built_gc *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c $(GC_COMMON_SRC) $(GC_SRC) $(PRECISE_GC_DIR)/gc.h $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h $(RUNTIME)/astro_debug.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -flto=auto -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            -DBARUBY_GC=$(GC_NUM) \
            -DBARUBY_PRECISE_DIR='"$(BARUBY_PRECISE_DIR)"' \
            -DASTRO_RUNTIME_DIR='"$(RUNTIME_ABSPATH)"' \
            -DASTRO_PRISM_INC_DIR='"$(PRISM_INC_ABSPATH)"' \
            $(if $(ASTRO_DEBUG),-DASTRO_DEBUG=$(ASTRO_DEBUG)) \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o baruby_precise main.c node.c node_slowpath.c baruby_parse.c astro_jit.c $(GC_COMMON_SRC) $(GC_SRC) -lprism -ldl

node_eval.c: ../../lib/* node.def baruby_gen.rb
	ruby -I ../../lib -r ./baruby_gen -e 'ASTroGen.start([], node_def_class: BaRubyNodeDef)'

