all: koruby_precise

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_emit_ast.c \
                node_walk.c \
                node_head.h

KORUBY_SRC_DIR := $(abspath .)
RUNTIME := ../../runtime
ASTRO_RUNTIME_DIR := $(abspath $(RUNTIME))

CC ?= gcc
optflags ?= -O2
PRISM_DIR = ./prism
PGO_DIR = $(CURDIR)/pgo-data

# Bignum backend (same as koruby).
MINI_GMP ?= 0
ifeq ($(MINI_GMP),1)
  GMP_CFLAGS = -DKORB_USE_MINI_GMP -Ivendor/mini-gmp
  # AOT variant: include path must be absolute (the code_store Makefile runs in code_store/).
  GMP_AOT_CFLAGS = -DKORB_USE_MINI_GMP -I$(KORUBY_SRC_DIR)/vendor/mini-gmp
  GMP_SRCS = vendor/mini-gmp/mini-gmp.c
  GMP_LIBS =
else
  GMP_CFLAGS =
  GMP_AOT_CFLAGS =
  GMP_SRCS =
  GMP_LIBS = -lgmp
endif

# GC backend selector — precise GC framework, see runtime/precise_gc/.
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
PRECISE_GC_DIR := $(RUNTIME)/precise_gc
GC_SRC := $(PRECISE_GC_DIR)/gc_$(GC).c
GC_COMMON_SRC := $(PRECISE_GC_DIR)/gc_common.c

ifeq ($(GC_NUM),)
$(error Unknown GC backend "$(GC)" — pick one of: none mark mark_gen mark_gen_inc copy copy_gen mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap_gen mark_card_gen mark_freelist)
endif

# Rebuild marker — tracks GC + audit selection so toggling forces rebuild.
GC_VARIANT := $(GC)$(if $(ARO_GC_WB_AUDIT),-audit)
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC_VARIANT))
$(shell echo "$(GC_VARIANT)" > .built_gc)
endif

# Flags handed to astro_cs_build() so the AOT-generated SD_*.c (compiled in
# code_store/) can resolve node.h's includes.  All paths absolute because the
# code_store Makefile runs in its own directory.  GC selector must match the
# host build or the SD body (which #includes node_eval.c) diverges from it.
KORUBY_AOT_CFLAGS := -I$(KORUBY_SRC_DIR) -I$(ASTRO_RUNTIME_DIR) -I$(KORUBY_SRC_DIR)/prism/include $(GMP_AOT_CFLAGS) -DBARUBY_GC=$(GC_NUM)

CFLAGS = -I$(PRISM_DIR)/include -I. -I$(RUNTIME) $(optflags) -flto=auto -ggdb3 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable -Werror=unused-result $(GMP_CFLAGS) \
         -DBARUBY_GC=$(GC_NUM) \
         $(if $(ARO_GC_WB_AUDIT),-DARO_GC_WB_AUDIT -Werror=discarded-qualifiers -Werror=cast-qual) \
         -DKORUBY_SRC_DIR_DEFAULT='"$(KORUBY_SRC_DIR)"' \
         -DKORUBY_AOT_CFLAGS_DEFAULT='"$(KORUBY_AOT_CFLAGS)"' \
         -DASTRO_RUNTIME_DIR='"$(ASTRO_RUNTIME_DIR)"'
LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(CURDIR)/$(PRISM_DIR)/build -flto=auto
LIBS = -lprism $(GMP_LIBS) -lm

SRCS = main.c node.c parse.c object.c builtins.c bootstrap_src.c koruby_runtime.c $(GMP_SRCS) $(GC_COMMON_SRC) $(GC_SRC)
BUILTINS_INCLUDED = $(wildcard builtins/*.c)
RUNTIME_INCLUDED = $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h $(PRECISE_GC_DIR)/gc.h

koruby_precise: .built_gc $(SRCS) $(BUILTINS_INCLUDED) $(RUNTIME_INCLUDED) $(NODE_GENFILES) Makefile
	$(CC) $(CFLAGS) -rdynamic -o $@ $(SRCS) $(LDFLAGS) $(LIBS) -ldl

$(NODE_GENFILES): node.def koruby_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KoRubyNodeDef)'

bootstrap_src.c: bootstrap.rb
	ruby -e 'src = File.binread("bootstrap.rb"); File.open("bootstrap_src.c","w") { |f| \
	  f.puts "/* Auto-generated from bootstrap.rb \xe2\x80\x94 do not edit. */"; \
	  f.puts "#include <stddef.h>"; f.puts "const char koruby_bootstrap_src[] = {"; \
	  src.each_byte.each_slice(20) { |s| f.puts s.map { |b| "0x%02x" % b }.join(",") + "," }; \
	  f.puts "0x00};"; \
	  f.puts "const size_t koruby_bootstrap_len = #{src.bytesize};" }'

clean:
	rm -fv $(NODE_GENFILES) koruby_precise .built_gc
	rm -rf pgo-data code_store

regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

run: koruby_precise
	./koruby_precise test.ko.rb

# ===== differential test harness (oracle = CRuby) — see t/README.md =====
# Corpus dirs: t/hand (hand-written), t/method (generated method surface),
#              t/syntax (generated + hand syntax).
# `gen` GENERATES the corpus; every `test*` target only RUNS it.
RUBY     ?= ruby
INTERP   ?= ./koruby_precise
JOBS     ?= $(shell nproc 2>/dev/null || echo 4)
# GC stress (runtime): STRESS=1 forces a GC at every allocation + PURGE
# (mprotect every page) so held-across-GC / stale-pointer bugs surface.
# Build-time GC backend is the separate `GC=<backend>` build var (see top).
STRESS   ?=
GC_STRESS_ENV = ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1
GCWRAP    = $(if $(STRESS),env $(GC_STRESS_ENV) ,)
TIMEOUT  ?= $(if $(STRESS),120,15)
# CAT=<area> focuses on one feature area across ALL corpora (hand+method+syntax),
# e.g. CAT=array / CAT=string / CAT=operator / CAT=exception.  Empty = every file.
CAT      ?=
PATTERN   = $(if $(CAT),'$(CAT)*.rb','*.rb')
RUNSPECS  = $(RUBY) tools/run_specs.rb --jobs $(JOBS) --timeout $(TIMEOUT)
# Only the interpreter under test gets the GC wrap; the CRuby oracle stays plain.
DIFF      = $(RUNSPECS) --pattern $(PATTERN) --interp "$(GCWRAP)$(INTERP)" --diff $(RUBY) --dir

# generate the auto corpus (run once / after editing the generators)
RUBYSPEC ?= $(HOME)/ruby/src/master/spec/ruby/core
gen:
	$(RUBY) tools/gen_golden.rb t/method 80
	$(RUBY) tools/gen_syntax.rb t/syntax 60
	@if test -d "$(RUBYSPEC)"; then \
	  $(RUBY) tools/gen_from_rubyspec.rb "$(RUBYSPEC)" t/spec 80; \
	else echo "  (skip rubyspec mining: $(RUBYSPEC) not found — set RUBYSPEC=)"; fi

# run the corpus against $(INTERP), each file diffed vs CRuby.
#   make test                  whole corpus (commit / CI)
#   make test CAT=array        one feature area only (fast, for dev loops)
#   make test STRESS=1         under GC stress (forces GC every alloc + PURGE)
#   make test INTERP=ruby      harness self-check (CRuby vs CRuby → all PASS)
#   make test GC=mark          build with a different GC backend, then run
# (knobs compose: e.g. make test CAT=array STRESS=1 GC=copy JOBS=4)
test: koruby_precise ; $(DIFF) t

.PHONY: all clean regen run test gen
