# Reusable Ruby differential test + benchmark harness (oracle = CRuby).
# Shared by the ASTro Ruby-subset samples (naruby / baruby / koruby_precise / …).
#
# Include from a Ruby sample's Makefile (after its own build rules):
#     INTERP ?= ./mysample
#     include ../rubyharness/harness.mk
#     test bench: mysample          # make the harness depend on the binary
#
# Provides targets: gen (generate corpus) / test (run) / bench (run).
# Knobs: INTERP= CAT=<area> STRESS=1 GC=<backend> JOBS=n TIMEOUT=s
#        BENCHRUNS=n BENCHMODES=a,b,c RUBYSPEC=path
# See t/README.md for details.

# Directory holding this .mk (so corpus/tools resolve regardless of the caller).
H        := $(dir $(lastword $(MAKEFILE_LIST)))
RUBY     ?= ruby
INTERP   ?= ./$(notdir $(CURDIR))
JOBS     ?= $(shell nproc 2>/dev/null || echo 4)

# GC stress (runtime): STRESS=1 wraps the interpreter so it GCs at every alloc.
# Override GC_STRESS_ENV per sample if its env-var names differ.
STRESS        ?=
GC_STRESS_ENV ?= ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1
GCWRAP         = $(if $(STRESS),env $(GC_STRESS_ENV) ,)
TIMEOUT       ?= $(if $(STRESS),120,15)

# CAT=<area> focuses on one feature area across all corpora (e.g. CAT=array).
CAT      ?=
PATTERN   = $(if $(CAT),'$(CAT)*.rb','*.rb')
RUNSPECS  = $(RUBY) $(H)tools/run_specs.rb --jobs $(JOBS) --timeout $(TIMEOUT)
DIFF      = $(RUNSPECS) --pattern $(PATTERN) --interp "$(GCWRAP)$(INTERP)" --diff $(RUBY) --dir
RUBYSPEC ?= $(HOME)/ruby/src/master/spec/ruby/core

# generate the auto corpus into the SHARED tree (run once / after editing generators)
gen:
	$(RUBY) $(H)tools/gen_golden.rb $(H)t/method 80
	$(RUBY) $(H)tools/gen_syntax.rb $(H)t/syntax 60
	@if test -d "$(RUBYSPEC)"; then \
	  $(RUBY) $(H)tools/gen_from_rubyspec.rb "$(RUBYSPEC)" $(H)t/spec 80; \
	else echo "  (skip rubyspec mining: $(RUBYSPEC) not found — set RUBYSPEC=)"; fi

# run the corpus against $(INTERP) (CAT=<area> to focus, STRESS=1 for GC stress,
# INTERP=ruby for the harness self-check).
test: ; $(DIFF) $(H)t

# micro-benchmarks across execution modes vs CRuby.
BENCHRUNS  ?= 5
BENCHMODES ?=
bench: ; $(RUBY) $(H)tools/run_bench.rb --interp "$(GCWRAP)$(INTERP)" --ref "$(RUBY)" \
	  --dir $(H)bench --runs $(BENCHRUNS) --timeout 180 $(if $(BENCHMODES),--modes $(BENCHMODES))

# pure-Ruby DOOM app benchmark (clone-on-demand, not committed; see tools/doom.sh).
# Renders FRAMES frames headless and prints a framebuffer checksum — CRuby and
# the sample must agree.  `make doom` runs the tree-walker, `make doom-aot` bakes
# then runs --compiled-only.  FRAMES= to scale, `make doom DOOM_MODE=cruby` for
# the oracle.  First run clones khasinski/doom + the shareware WAD.
FRAMES ?= 60
doom:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" FRAMES=$(FRAMES) DOOM_MODE=$(if $(DOOM_MODE),$(DOOM_MODE),plain) sh $(H)tools/doom.sh
doom-aot: ; INTERP="$(INTERP)" RUBY="$(RUBY)" FRAMES=$(FRAMES) DOOM_MODE=aot sh $(H)tools/doom.sh

# ruby/ruby-bench single-file micros (clone-on-demand, not committed).  koruby
# has no `require`, so tools/rubybench.sh bundles a run_benchmark shim + the
# bench and prints its (deterministic) result — CRuby and the sample must agree.
# `make rubybench BENCH=fib` runs one (RB_MODE=plain|aot|cruby|cruby-yjit,
# BENCH_ITRS= to scale); `make rubybench-all` sweeps every micro for correctness.
rubybench:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" BENCH="$(BENCH)" RB_MODE=$(if $(RB_MODE),$(RB_MODE),plain) BENCH_ITRS=$(if $(BENCH_ITRS),$(BENCH_ITRS),1) sh $(H)tools/rubybench.sh
rubybench-all: ; INTERP="$(INTERP)" RUBY="$(RUBY)" sh $(H)tools/rubybench_sweep.sh

# pure-Ruby Game Boy emulator app benchmark (sacckey/rubyboy, clone-on-demand,
# not committed; see tools/rubyboy.sh).  Runs EmulatorHeadless for FRAMES frames
# on tobu.gb and prints a framebuffer checksum — CRuby and the sample must agree.
# `make rubyboy` runs the tree-walker, `make rubyboy-aot` bakes the (bundled)
# engine then runs --compiled-only.  FRAMES= to scale, RB_MODE=cruby|cruby-yjit
# for the oracle.  First run clones sacckey/rubyboy (ROM ships in the repo).
rubyboy:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" FRAMES=$(FRAMES) RB_MODE=$(if $(RB_MODE),$(RB_MODE),plain) sh $(H)tools/rubyboy.sh
rubyboy-aot: ; INTERP="$(INTERP)" RUBY="$(RUBY)" FRAMES=$(FRAMES) RB_MODE=aot sh $(H)tools/rubyboy.sh

# pure-Ruby Go (Baduk) Monte-Carlo AI benchmark (PragTob/rubykon, shipped in
# ruby/ruby-bench; clone-on-demand).  Seeded MCTS batch (multi-file require) →
# best-move checksum; CRuby and the sample must agree.  `make rubykon` runs the
# tree-walker, `make rubykon-aot` bakes (--aot-compile --run) then --compiled-only.
# GAMES=/ITERS=/SIZE= to scale, RB_MODE=cruby|cruby-yjit for the oracle.
rubykon:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=$(if $(RB_MODE),$(RB_MODE),plain) sh $(H)tools/rubykon.sh
rubykon-aot: ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=aot sh $(H)tools/rubykon.sh

# pure-Ruby JSON parser benchmark (ruby/ruby-bench "ruby-json"; StringScanner-
# based, no C json ext).  Parses data.json ITRS times → checksum; CRuby and the
# sample must agree.  Exercises koruby's lib/strscan.rb + lib/json.rb + Regexp
# captures over ASCII-8BIT.  make ruby-json / ruby-json-aot, ITRS= to scale.
ruby-json:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=$(if $(RB_MODE),$(RB_MODE),plain) sh $(H)tools/ruby_json.sh
ruby-json-aot: ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=aot sh $(H)tools/ruby_json.sh

# pure-Ruby Protocol Buffers benchmark (ruby/ruby-bench "protoboeuf"; generated
# pure-Ruby codec, no C ext).  Decodes a fixed message set and re-encodes it
# ITRS times → checksum; CRuby and the sample must agree (byte-exact decode +
# encode).  Exercises Marshal.load, Array#pack(buffer:), String#<<(int) on
# ASCII-8BIT.  make protoboeuf / protoboeuf-aot, ITRS= to scale.
protoboeuf:     ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=$(if $(RB_MODE),$(RB_MODE),plain) sh $(H)tools/protoboeuf.sh
protoboeuf-aot: ; INTERP="$(INTERP)" RUBY="$(RUBY)" RB_MODE=aot sh $(H)tools/protoboeuf.sh

# remove the generated corpus (t/method, t/spec, generated t/syntax) + code_store;
# hand-written tests are kept.  Regenerate with `make gen`.
clean-corpus:
	rm -rf $(H)t/method $(H)t/spec code_store
	@find $(H)t/syntax -name '*.rb' ! -name 'hand_*' -delete 2>/dev/null || true

.PHONY: gen test bench clean-corpus doom doom-aot rubybench rubybench-all rubyboy rubyboy-aot rubykon rubykon-aot ruby-json ruby-json-aot protoboeuf protoboeuf-aot
