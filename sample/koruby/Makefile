all: koruby

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
PRISM_DIR = ./prism
CFLAGS = -I$(PRISM_DIR)/include -I. $(optflags) -ggdb3 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable
LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(CURDIR)/$(PRISM_DIR)/build
LIBS = -lprism -lgc -lgmp -lm

SRCS = main.c node.c parse.c object.c builtins.c

koruby: $(SRCS) $(NODE_GENFILES) Makefile
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

# generated files come from node.def via koruby_gen.rb
$(NODE_GENFILES): node.def koruby_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./koruby_gen -e 'ASTroGen.start([], node_def_class: KoRubyNodeDef)'

# specialized.c: empty stub on first build
node_specialized.c:
	touch -t 200001010000 node_specialized.c

clean:
	rm -fv $(NODE_GENFILES) node_specialized.c koruby compiled_koruby

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

.PHONY: all clean regen run bench-fib compiled_koruby
