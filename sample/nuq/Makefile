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
LIBS = -ldl -lm

nuq: main.c node.c runtime.c value.c json.c filter.c builtin.c linearity.c Makefile $(NODE_GENFILES) context.h node.h
	$(CC) $(CFLAGS) -o nuq main.c node.c runtime.c value.c json.c filter.c builtin.c linearity.c $(LIBS)

$(NODE_GENFILES): node.def nuq_gen.rb ../../lib/astrogen.rb
	ruby -I ../../lib -r ./nuq_gen -e 'ASTroGen.start([], node_def_class: NuqNodeDef)'

test: nuq
	ruby test/run_tests.rb

# GC bug catcher: every arena alloc forces a Cheney pass, and
# from-space pages are mprotect(PROT_NONE)+madvise(DONTNEED) so any
# stale arena pointer SEGVs at the deref site instead of silently
# reading garbage from a recycled chunk.  Slow.  Use to flush out
# missing NUQ_GC_PIN sites the production builds wouldn't catch.
nuq-gcdebug: main.c node.c runtime.c value.c json.c filter.c builtin.c linearity.c Makefile $(NODE_GENFILES) context.h node.h
	$(CC) $(CFLAGS) -DNUQ_GC_DEBUG_MPROTECT=1 -DNUQ_GC_DEBUG_STRESS=1 -O0 -o nuq-gcdebug main.c node.c runtime.c value.c json.c filter.c builtin.c linearity.c $(LIBS)

gctest: nuq-gcdebug
	NUQ=$(PWD)/nuq-gcdebug ruby test/run_tests.rb

# Run jq's official tests/jq.test against nuq (downloads on first
# use to /tmp/claude/jq.test or JQ_TESTFILE).  Tells you nuq's
# compatibility score with jq's master test suite.
jqtest: nuq
	ruby test/run_jq_official.rb

bench: nuq
	ruby bench/bench.rb

# Real-world NDJSON dataset for the `jsonl` bench suite.  Downloads
# a 1-hour GitHub Archive slice (~120 MB compressed), decompresses
# and truncates to 30K events (~100 MB).  Skipped if file is already
# present.  Network access required on first run only.
bench-data-jsonl:
	ruby bench/data/gen_jsonl.rb

bench-jsonl: nuq bench-data-jsonl
	ruby bench/bench.rb jsonl

clean:
	rm -f nuq $(NODE_GENFILES)
	rm -rf code_store

# Wipe generated bench data (forces re-download / re-generation on
# next run).  Not part of `clean` since download is slow.
clean-bench-data:
	rm -rf bench/data/big bench/data/jsonl/*.jsonl bench/data/jsonl/*.json bench/data/jsonl/*.json.gz

distclean: clean clean-bench-data
	rm -rf code_store

.PHONY: all test gctest jqtest bench bench-data-jsonl bench-jsonl clean clean-bench-data distclean
