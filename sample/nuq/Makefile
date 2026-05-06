all: nuq

NODE_GENFILES = node_eval.c \
                node_dispatch.c \
                node_dump.c \
                node_specialize.c \
                node_replace.c \
                node_alloc.c \
                node_hash.c \
                node_head.h

RUNTIME = ../../runtime
optflags = -O2
CC = gcc
ASTROGEN = ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

CFLAGS = -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -rdynamic -D_GNU_SOURCE
LIBS = -ldl -lm -lgc

nuq: main.c node.c runtime.c value.c json.c filter.c builtin.c Makefile $(NODE_GENFILES) context.h node.h
	$(CC) $(CFLAGS) -o nuq main.c node.c runtime.c value.c json.c filter.c builtin.c $(LIBS)

$(NODE_GENFILES): node.def nuq_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./nuq_gen -e 'ASTroGen.start([], node_def_class: NuqNodeDef)'

test: nuq
	ruby test/run_tests.rb

# Run jq's official tests/jq.test against nuq (downloads on first
# use to /tmp/claude/jq.test or JQ_TESTFILE).  Tells you nuq's
# compatibility score with jq's master test suite.
jqtest: nuq
	ruby test/run_jq_official.rb

bench: nuq
	ruby bench/bench.rb

clean:
	rm -f nuq $(NODE_GENFILES)
	rm -rf code_store

distclean: clean
	rm -rf code_store

.PHONY: all test jqtest bench clean distclean
