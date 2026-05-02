
all: naruby

.PHONY: crun bench clean clean_bench

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

RUNTIME=../../runtime

optflags=-O3
BITEM=fib
ITEM=test.na.rb
CC=gcc

clean_bench:
	rm -fv $(NODE_GENFILES) naruby b0 b1 b2 b3 b_spinel
	rm -rf code_store

clean: clean_bench

NARUBY_OPT= -j

run: naruby
	time -p ./naruby $(NARUBY_OPT) test.na.rb

runb:
	./naruby -b test.na.rb

crun:
	make clean
	make run

ptrain:
	make clean
	make naruby
	./naruby $(ITEM) -p
	rm -f naruby
	make naruby

ctrain:
	make clean
	make naruby
	./naruby -c $(ITEM)
	rm -f naruby
	make naruby

gdb: naruby
	gdb -ex run --args ./naruby $(NARUBY_OPT) test.na.rb

c: naruby
	./naruby -c test.na.rb

cc_opt= bench/f0.c bench/f1.c bench/f2.c bench/f3.c bench/f4.c bench/f5.c bench/f6.c bench/f7.c bench/f8.c bench/f9.c
# -fno-inline -fno-ipa-sra -fno-ipa-cp -fno-early-inlining -fno-tree-sra -fno-tree-dce -fno-tree-ccp -fno-tree-fre -fno-tree-ch -fno-tree-dominator-opts -fno-tree-ter -fno-tree-pre
# -finline-functions-called-once -fno-inline-functions -fno-inline -fno-ipa-pure-const

bench_c: Makefile bench/*.c
	$(CC) -O0 $(cc_opt) bench/$(BITEM).c -o b0
	$(CC) -O1 $(cc_opt) bench/$(BITEM).c -o b1
	$(CC) -O2 $(cc_opt) bench/$(BITEM).c -o b2
	$(CC) -O3 $(cc_opt) bench/$(BITEM).c -o b3

# Benchmark runner.  Each variant (plain / aot / pg) is a single
# invocation of ./naruby with a different option mix; bench.rb does its
# own untimed code_store warm-up before the timed `aot` / `pg` rows so
# the first-run dlopen is amortized.
bench: naruby
	make bench_c BITEM=$(BITEM)
	ruby bench/bench.rb $(BITEM)

naruby: *.c Makefile $(NODE_GENFILES) astro_jit.c node.c node_slowpath.c $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c $(RUNTIME)/astro_code_store.h
	$(CC) -I./prism/include -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall  -rdynamic \
            -fno-plt -march=native \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o naruby main.c node.c node_slowpath.c naruby_parse.c astro_jit.c -lprism -ldl

node_eval.c: ../../lib/* node.def naruby_gen.rb
	ruby -I ../../lib -r ./naruby_gen -e 'ASTroGen.start([], node_def_class: NaRubyNodeDef)'

