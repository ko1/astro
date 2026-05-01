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

SRCS = main.c node.c parse.c object.c builtins.c

koruby: $(SRCS) $(NODE_GENFILES) Makefile
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

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
	$(CC) $(CFLAGS) -fprofile-generate=$(PGO_DIR) -o koruby $(SRCS) $(LDFLAGS) $(LIBS) -fprofile-generate=$(PGO_DIR)
	@echo "==> Running optcarrot to collect profile data"
	./koruby $(PGO_BENCH) $(PGO_BENCH_ARGS) >/dev/null 2>&1
	@echo "==> Pass 2: rebuild with profile data"
	$(CC) $(CFLAGS) -fprofile-use=$(PGO_DIR) -fprofile-correction -o koruby $(SRCS) $(LDFLAGS) $(LIBS)
	@echo "==> PGO build done"

# generated files come from node.def via koruby_gen.rb
$(NODE_GENFILES): node.def koruby_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KoRubyNodeDef)'

# specialized.c: empty stub on first build
node_specialized.c:
	touch -t 200001010000 node_specialized.c

clean:
	rm -fv $(NODE_GENFILES) node_specialized.c koruby compiled_koruby
	rm -rf pgo-data

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

.PHONY: all clean regen run bench-fib compiled_koruby test
