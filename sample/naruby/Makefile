
all: naruby

.PHONY: compiled_naruby pg_naruby crun bench clean clean_bench

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_specialized.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

optflags=-O3
BITEM=fib
ITEM=test.na.rb
CC=gcc

clean_bench:
	rm -fv $(NODE_GENFILES) naruby b0 b1 b2 b3

clean: clean_bench
	rm -fv compiled_* pg_* static_*

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

compiled_naruby:
	rm -fv node_specialized.c
	make naruby
	./naruby -c bench/$(BITEM).na.rb # get node_specialized.c
	cp node_specialized.c compiled_node_specialized.c
	$(CC) -I./prism/include -I. $(optflags) -ggdb3 -Wall \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o compiled_naruby main.c node.c naruby_parse.c -lprism

pg_naruby:
	rm -fv node_specialized.c
	make naruby
	./naruby -p bench/$(BITEM).na.rb # get node_specialized.c with running
	cp node_specialized.c pg_node_specialized.c
	$(CC) -I./prism/include -I. $(optflags) -ggdb3 -Wall \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o pg_naruby main.c node.c naruby_parse.c -lprism \
            --param=early-inlining-insns=100

static_naruby:
	rm -fv node_specialized.c
	make naruby
	./naruby -s bench/$(BITEM).na.rb # get node_specialized.c with running
	cp node_specialized.c static_node_specialized.c
	$(CC) -I./prism/include -I. $(optflags) -ggdb3 -Wall \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o static_naruby main.c node.c naruby_parse.c -lprism

cc_opt= bench/f0.c bench/f1.c bench/f2.c bench/f3.c bench/f4.c bench/f5.c bench/f6.c bench/f7.c bench/f8.c bench/f9.c
# -fno-inline -fno-ipa-sra -fno-ipa-cp -fno-early-inlining -fno-tree-sra -fno-tree-dce -fno-tree-ccp -fno-tree-fre -fno-tree-ch -fno-tree-dominator-opts -fno-tree-ter -fno-tree-pre
# -finline-functions-called-once -fno-inline-functions -fno-inline -fno-ipa-pure-const

bench_c: Makefile bench/*.c
	$(CC) -O0 $(cc_opt) bench/$(BITEM).c -o b0
	$(CC) -O1 $(cc_opt) bench/$(BITEM).c -o b1
	$(CC) -O2 $(cc_opt) bench/$(BITEM).c -o b2
	$(CC) -O3 $(cc_opt) bench/$(BITEM).c -o b3

bench: naruby
	rm -f static_naruby compiled_naruby pg_naruby
	make static_naruby   BITEM=$(BITEM)
	make compiled_naruby BITEM=$(BITEM)
	make pg_naruby       BITEM=$(BITEM)
	make clean_bench
	make naruby # for interpreter
	make bench_c         BITEM=$(BITEM)
	ruby bench/bench.rb $(BITEM)

naruby: *.c Makefile $(NODE_GENFILES) astro_jit.c node.c
	$(CC) -I./prism/include -I. $(optflags) -ggdb3 -Wall  -rdynamic \
            -L ./prism/build -Wl,-rpath=$(CURDIR)/prism/build \
            -o naruby main.c node.c naruby_parse.c astro_jit.c -lprism

node_eval.c: ../../lib/* node.def naruby_gen.rb
	ruby -I ../../lib -r ./naruby_gen -e 'ASTroGen.start([], node_def_class: NaRubyNodeDef)'

node_specialized.c:
	touch -t 200001010000 node_specialized.c

