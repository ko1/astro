# koruby_precise v2 — build (docs/v2_spec.md §5).
#
# Sources are explicitly listed (no *.c glob) and the GC backend selection
# is tracked through a marker file so `make GC=foo` after `make GC=bar`
# relinks even when no source changed.

all: koruby_precise koruby_regex.so

.PHONY: all run clean

# Regexp engine — koruby's thin bridge links libastrogre.so (astrogre's generic
# ASTro symbols stay local to that library, so they never clash with koruby's).
# koruby dlopen()s koruby_regex.so RTLD_LOCAL; an absolute rpath lets its loader
# find libastrogre.so at runtime.
ASTROGRE = ../astrogre
ASTROGRE_ABS := $(abspath $(ASTROGRE))
$(ASTROGRE)/libastrogre.so: $(ASTROGRE)/parse.c $(ASTROGRE)/node.c $(ASTROGRE)/match.c \
                            $(ASTROGRE)/backend_astrogre.c $(ASTROGRE)/aho_corasick.c $(ASTROGRE)/node.def \
                            $(ASTROGRE)/unicode_prop.c $(ASTROGRE)/unicode_tables.c \
                            $(ASTROGRE)/unicode_prop.h $(ASTROGRE)/unicode_gcb.h \
                            $(ASTROGRE)/unicode_grapheme.h
	$(MAKE) -C $(ASTROGRE) lib
koruby_regex.so: regex_bridge.c $(ASTROGRE)/libastrogre.so
	cc -shared -fPIC -O2 -mavx2 -I$(ASTROGRE) -I$(RUNTIME) \
	    regex_bridge.c \
	    -L$(ASTROGRE) -lastrogre -Wl,-rpath,$(ASTROGRE_ABS) \
	    -o koruby_regex.so

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

# 多倍長整数 backend (builtins/bignum_backend.h): gmp = 真の任意精度 (既定),
# wrap = Bignum 非対応 (外部依存なし)。fixnum (±2^62) 内は CRuby と一致、超えると
# 結果は保証されない (2**62→0 等; wasi/README.md 参照)
BIGNUM ?= gmp
BIGNUM_NUM_gmp  := 1
BIGNUM_NUM_wrap := 2
BIGNUM_NUM := $(BIGNUM_NUM_$(BIGNUM))
ifeq ($(BIGNUM_NUM),)
  $(error unknown BIGNUM=$(BIGNUM) (gmp|wrap))
endif
BIGNUM_LIBS_gmp  := -lgmp
BIGNUM_LIBS_wrap :=
BIGNUM_LIBS := $(BIGNUM_LIBS_$(BIGNUM))

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

# Marker file: rebuild when the GC / bignum selection (or debug level) changes.
GC_VARIANT := $(GC)-b$(BIGNUM)-d$(ASTRO_DEBUG)
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
BUILTINS_INCLUDED = $(wildcard builtins/*.c) $(wildcard builtins/*.h)

KORUBY_DEPS = .built_gc Makefile $(SRCS) context.h node.h korb_runtime.h $(BUILTINS_INCLUDED) $(NODE_GENFILES) \
              $(GC_COMMON_SRC) $(GC_SRC) $(PRECISE_GC_DIR)/gc.h $(PRECISE_GC_DIR)/gc_types.h \
              $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h \
              $(RUNTIME)/astro_build.c $(RUNTIME)/astro_build.h $(RUNTIME)/astro_ref_template.h
KORUBY_CPPFLAGS = -I. -I./prism/include -I$(RUNTIME) \
	    -DBARUBY_GC=$(GC_NUM) \
	    -DASTRO_DEBUG=$(ASTRO_DEBUG) \
	    -DKORUBY_SRC_DIR='"$(KORUBY_SRC_DIR)"' \
	    -DASTRO_RUNTIME_DIR='"$(RUNTIME_ABSPATH)"' \
	    -DASTRO_PRISM_INC_DIR='"$(PRISM_INC_ABSPATH)"' \
	    -DKORB_BIGNUM=$(BIGNUM_NUM) \
	    -DKORUBY_GC_SRC='"$(RUNTIME_ABSPATH)/precise_gc/gc_$(GC).c"'
# zlib is optional: probe for the header and link it only when present, so a
# build without it just leaves `require "zlib"` a LoadError.
ZLIB_OK   := $(shell $(CC) -E -include zlib.h -xc /dev/null >/dev/null 2>&1 && echo yes)
ZLIB_FLAGS := $(if $(ZLIB_OK),-DKORB_HAVE_ZLIB -lz,)

# $@ = output binary; $(1) = extra flags/sources
define KORUBY_LINK
	$(CC) $(KORUBY_CPPFLAGS) $(optflags) -flto=auto -ggdb3 -Wall -Wextra \
	    -Wno-unused-parameter -rdynamic -fno-plt \
	    -o $@ $(SRCS) $(1) $(GC_COMMON_SRC) $(GC_SRC) ./prism/build/libprism.a -ldl -lm $(BIGNUM_LIBS) -lcrypt $(ZLIB_FLAGS)
endef

koruby_precise: $(KORUBY_DEPS)
	$(call KORUBY_LINK)

# Generated files: grouped target (single generator invocation).
$(NODE_GENFILES) &: node.def koruby_gen.rb $(LIB)/astrogen.rb
	ruby -I $(LIB) -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KorubyNodeDef)'

run: koruby_precise
	./koruby_precise -e 'def fib(n) = n < 2 ? n : fib(n-1) + fib(n-2); puts fib(25)'

# Native-memory leak regression check (mpz limbs, class method tables, ...).
# Churns leak-prone objects under GC stress in valgrind; asserts the
# definitely-lost block count stays small.  Skips cleanly without valgrind.
leak-check: koruby_precise
	tools/leak_check.sh ./koruby_precise
.PHONY: leak-check

clean:
	rm -f koruby_precise $(NODE_GENFILES) .built_gc
	rm -rf code_store

# CodeQL GC-borrow gate (docs/c_ext_api_design.md §4.1).  Run after changes:
# builds a DB from the koruby build trace and fails if any String/Array byte-
# buffer pointer is held in a local across a may-GC call.  Also self-tests the
# query against codeql/test/borrow_cases.c (must find its 2 injected bugs).
# Needs the CodeQL CLI: gh extension install github/gh-codeql.
codeql-check:
	sh codeql/run.sh
.PHONY: codeql-check

# rubyharness (shared differential tests + bench)
INTERP ?= ./koruby_precise
# koruby reads BARUBY_GC_* (not the harness default ASTRO_GC_*), so override the
# stress env BEFORE the include (harness uses ?=) — otherwise `make test STRESS=1`
# would set the wrong vars and silently NOT stress (green but no GC audit).
GC_STRESS_ENV := BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1
include ../rubyharness/harness.mk
test bench: koruby_precise

# Real rubyspec compat gate: run the actual CRuby core specs through the mspec
# shim and fail on any regression vs tools/rubyspec_baseline.txt (encoding/regex/
# IO stay failing by design — the baseline encodes the supported set).  This
# tests behavioural contracts (frozen?/equal?/raises/warnings/mocks) that the
# generated `p (expr)` golden can't.  `make rubyspec-update` re-records the
# baseline after reviewing the reported improvements.
.PHONY: rubyspec rubyspec-update
rubyspec: koruby_precise
	@$(RUBY) tools/rubyspec_gate.rb
rubyspec-update: koruby_precise
	@$(RUBY) tools/rubyspec_gate.rb --update

# Persist a full benchmark sweep to bench-report/<timestamp>-<commit>.txt.
# (The shared `bench` target only prints to stdout.)  Knobs: BENCHRUNS=, BENCHMODES=.
BENCHREPORT_MODES ?= $(if $(BENCHMODES),$(BENCHMODES),cruby,cruby+yjit,interp,aot+compile,aot+cached)
bench-report: koruby_precise
	@mkdir -p bench-report
	@f="bench-report/$$(date +%Y%m%d-%H%M%S)-$$(git rev-parse --short HEAD 2>/dev/null || echo nogit).txt"; \
	{ printf '# koruby_precise bench  %s  %s\n# modes: %s   runs: %s   host: %s\n\n' \
	    "$$(date '+%Y-%m-%d %H:%M:%S')" "$$(git rev-parse --short HEAD 2>/dev/null || echo nogit)" \
	    "$(BENCHREPORT_MODES)" "$(BENCHRUNS)" "$$(uname -sr)"; \
	  $(RUBY) $(H)tools/run_bench.rb --interp "$(GCWRAP)$(INTERP)" --ref "$(RUBY)" \
	    --dir $(H)bench --runs $(BENCHRUNS) --timeout 180 --modes $(BENCHREPORT_MODES) \
	    --aot-flags --compiled-only; \
	} | tee "$$f"; \
	echo "saved: $$f"
.PHONY: bench-report

# optcarrot (NES emulator) — the real-workload benchmark.  FRAMES= adjusts the
# frame count (default 180 ≈ a ~1s+ sustained run).  `optcarrot` = pure
# tree-walker (--plain); `optcarrot-aot` = bake + --compiled-only (AOT FPS).
FRAMES ?= 180
GCROUNDS ?= 7
optcarrot: koruby_precise
	tools/optcarrot.sh $(FRAMES)
optcarrot-aot: koruby_precise
	OPTC_MODE=aot tools/optcarrot.sh $(FRAMES)
# Same modes as bench-report (cruby/cruby+yjit/interp/aot+cached) but for the
# optcarrot workload, reporting BOTH wall time and the emulator fps.  Saved to
# bench-report/.  Knobs: FRAMES=, BENCHRUNS=.
optcarrot-report: koruby_precise
	@mkdir -p bench-report
	@f="bench-report/$$(date +%Y%m%d-%H%M%S)-$$(git rev-parse --short HEAD 2>/dev/null || echo nogit)-optcarrot.txt"; \
	{ printf '# koruby_precise optcarrot  %s  %s\n# frames: %s   runs: %s   host: %s\n\n' \
	    "$$(date '+%Y-%m-%d %H:%M:%S')" "$$(git rev-parse --short HEAD 2>/dev/null || echo nogit)" \
	    "$(FRAMES)" "$(BENCHRUNS)" "$$(uname -sr)"; \
	  tools/optcarrot_bench.sh $(FRAMES) $(BENCHRUNS); \
	} | tee "$$f"; \
	echo "saved: $$f"
# GC backend comparison for optcarrot.  gc-compare = rigorous alternating
# round-robin A/B (robust to fluctuating machine load), reports best+median fps
# per backend.  But the load-independent answer is gc-stats: run optcarrot AOT
# per backend with KORUBY_GC_STATS=1 — for optcarrot GC is only 1-4% of runtime
# (3-17 collections / ~1MB per frame), so the collector algorithm barely matters
# and generational backends are slightly worse (smaller nursery -> more minors).
GC_BACKENDS ?= copy copy_gen immix immix_gen mark_gen
gc-compare:
	tools/gc_ab.sh $(FRAMES) $(GCROUNDS) $(GC_BACKENDS)
.PHONY: optcarrot optcarrot-aot optcarrot-report gc-compare
