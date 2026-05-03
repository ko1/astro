all: koruby

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
PRISM_DIR = ./prism
PGO_DIR = $(CURDIR)/pgo-data
CFLAGS = -I$(PRISM_DIR)/include -I. $(optflags) -flto=auto -ggdb3 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable
LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(CURDIR)/$(PRISM_DIR)/build -flto=auto
LIBS = -lprism -lgc -lgmp -lm

SRCS = main.c node.c parse.c object.c builtins.c bootstrap_src.c
# builtins/*.c are #included from builtins.c, but list them here so
# `make` rebuilds when any of them changes.  Same for the shared
# runtime files included from node.c.
BUILTINS_INCLUDED = $(wildcard builtins/*.c)
RUNTIME_INCLUDED = ../../runtime/astro_node.c ../../runtime/astro_code_store.c ../../runtime/astro_code_store.h

koruby: $(SRCS) $(BUILTINS_INCLUDED) $(RUNTIME_INCLUDED) $(NODE_GENFILES) Makefile
	$(CC) $(CFLAGS) -rdynamic -o $@ $(SRCS) $(LDFLAGS) $(LIBS) -ldl

# Profile-guided optimization build.  Two-pass:
# 1) build with -fprofile-generate, run optcarrot to collect profile
# 2) rebuild with -fprofile-use using the collected data
# Result: ~15-20% faster on optcarrot than plain -O2 alone.
PGO_BENCH = ../abruby/benchmark/optcarrot/bin/optcarrot-bench
# Cover warmup + steady state of PPU/CPU dispatch loops.
PGO_BENCH_ARGS = --frames 60 --headless --no-print-fps --no-print-video-checksum

koruby-pgo: $(SRCS) $(NODE_GENFILES) Makefile
	@echo "==> Pass 1: instrumented build for profile collection"
	mkdir -p $(PGO_DIR)
	$(CC) $(CFLAGS) -fprofile-generate=$(PGO_DIR) -rdynamic -o koruby $(SRCS) $(LDFLAGS) $(LIBS) -ldl -fprofile-generate=$(PGO_DIR)
	@echo "==> Running optcarrot to collect profile data"
	./koruby $(PGO_BENCH) $(PGO_BENCH_ARGS) >/dev/null 2>&1
	@echo "==> Pass 2: rebuild with profile data"
	$(CC) $(CFLAGS) -fprofile-use=$(PGO_DIR) -fprofile-correction -rdynamic -o koruby $(SRCS) $(LDFLAGS) $(LIBS) -ldl
	@echo "==> PGO build done"

# AOT (ahead-of-time specialization) build, abruby-style.
# 1) Build koruby normally
# 2) Run optcarrot under --aot-compile: this runs the program once,
#    then for every entry (toplevel + each method body) writes
#    code_store/c/SD_<hash>.c and builds code_store/all.so.
# 3) Subsequent runs auto-detect code_store/all.so via dlopen.
#
# Result: ~25% faster on optcarrot than -O2 + LTO alone (54 fps vs 43).
AOT_BENCH = ../abruby/benchmark/optcarrot/bin/optcarrot-bench
AOT_BENCH_ARGS = --frames 30 --headless --no-print-fps --no-print-video-checksum

koruby-aot: koruby
	rm -rf code_store
	@echo "==> Running optcarrot under --aot-compile"
	./koruby --aot-compile $(AOT_BENCH) $(AOT_BENCH_ARGS) 2>&1 | tail -3
	@echo "==> AOT build done.  code_store/all.so:"
	@ls -la code_store/all.so

# PGO + AOT: both layers profile-driven.  Rebuilds koruby via koruby-pgo
# (instrument → run → re-link with profile data), then generates the AOT
# code_store/ on top of the PGO'd koruby.  Optcarrot: ~80 fps median vs
# ~71 fps for AOT alone.
koruby-pgo-aot: koruby-pgo
	rm -rf code_store
	@echo "==> Running optcarrot under --aot-compile (on PGO'd koruby)"
	./koruby --aot-compile $(AOT_BENCH) $(AOT_BENCH_ARGS) 2>&1 | tail -3
	@echo "==> PGO+AOT build done.  code_store/all.so:"
	@ls -la code_store/all.so

# AOT-level PGO: profile-guide all.so itself.  This is the bigger win
# (~+6 fps optcarrot, 105→112).  Three passes on the AOT layer:
#   1. Build all.so with -fprofile-generate (instrumented .o + linked .so)
#   2. Run optcarrot once to write .gcda files into AOT_PGO_DIR
#   3. Rebuild all.so with -fprofile-use using the collected profile
# koruby itself is built normally.  Project rule (-fno-lto on all.so) is
# preserved by passing LDFLAGS=-fno-lto to both compile-link steps.
AOT_PGO_DIR = $(CURDIR)/aot-pgo-data
AOT_PGO_FRAMES = 120
koruby-aot-pgo: koruby
	@echo "==> AOT-PGO pass 1: instrumented all.so build"
	rm -rf code_store $(AOT_PGO_DIR)
	mkdir -p $(AOT_PGO_DIR)
	CC="$(CC)" \
	CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-generate=$(AOT_PGO_DIR)" \
	LDFLAGS="-fno-lto -fprofile-generate=$(AOT_PGO_DIR)" \
	./koruby --aot-compile $(AOT_BENCH) $(AOT_BENCH_ARGS)
	@echo "==> AOT-PGO pass 2: collect profile (optcarrot $(AOT_PGO_FRAMES) frames)"
	./koruby $(AOT_BENCH) --frames $(AOT_PGO_FRAMES) --headless \
	    --no-print-fps --no-print-video-checksum >/dev/null
	@n=$$(find $(AOT_PGO_DIR) -name '*.gcda' | wc -l); \
	  echo "==> .gcda files written: $$n"; \
	  test "$$n" -gt 0
	@echo "==> AOT-PGO pass 3: rebuild all.so with -fprofile-use"
	rm -f code_store/o/*.o code_store/all.so
	CC="$(CC)" \
	CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-use=$(AOT_PGO_DIR) -fprofile-correction" \
	LDFLAGS="-fno-lto" \
	$(MAKE) -C code_store -s all.so
	@echo "==> AOT-PGO build done.  code_store/all.so:"
	@ls -la code_store/all.so

# generated files come from node.def via koruby_gen.rb
$(NODE_GENFILES): node.def koruby_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KoRubyNodeDef)'

# bootstrap_src.c — embedded Ruby source loaded at startup (Enumerable,
# Comparable wiring, Rational/Complex, helper methods).
bootstrap_src.c: bootstrap.rb
	ruby -e 'src = File.binread("bootstrap.rb"); File.open("bootstrap_src.c","w") { |f| \
	  f.puts "/* Auto-generated from bootstrap.rb \xe2\x80\x94 do not edit. */"; \
	  f.puts "#include <stddef.h>"; f.puts "const char koruby_bootstrap_src[] = {"; \
	  src.each_byte.each_slice(20) { |s| f.puts s.map { |b| "0x%02x" % b }.join(",") + "," }; \
	  f.puts "0x00};"; \
	  f.puts "const size_t koruby_bootstrap_len = #{src.bytesize};" }'

# specialized.c: empty stub on first build
node_specialized.c:
	touch -t 200001010000 node_specialized.c

clean:
	rm -fv $(NODE_GENFILES) node_specialized.c koruby compiled_koruby
	rm -rf pgo-data code_store

# regenerate (force rebuild)
regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

run: koruby
	./koruby test.ko.rb

# Two-pass: 1st pass collects specialization C source; 2nd pass compiles it in.
compiled_koruby: koruby
	rm -f node_specialized.c
	./koruby -c $(BENCH)
	$(CC) $(CFLAGS) -O3 -o compiled_koruby $(SRCS) $(LDFLAGS) $(LIBS)

bench-fib: koruby
	@echo "=== koruby (interp) ==="
	@time -p ./koruby fib.ko.rb
	@echo
	@echo "=== ruby --yjit ==="
	@time -p ruby --yjit fib.ko.rb || true

test: koruby
	@for f in test/test_*.rb; do \
	    ./koruby "$$f" 2>&1 | tail -1; \
	done

bench-optcarrot: koruby
	@bash tools/bench-optcarrot.sh

.PHONY: all clean regen run bench-fib bench-optcarrot compiled_koruby test koruby-pgo koruby-aot koruby-pgo-aot koruby-aot-pgo
