
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

FRONTEND_PARTS = wat_tokenizer.c \
                 host_imports.c \
                 wat_parser.c \
                 wasm_decoder.c \
                 wast_runner.c

wastro: main.c node.c Makefile $(NODE_GENFILES) $(FRONTEND_PARTS) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall \
	    -rdynamic \
	    -o wastro main.c node.c -ldl -lm

node_eval.c: ../../lib/* node.def wastro_gen.rb
	ruby -I ../../lib -I . -r astrogen -r wastro_gen \
	    -e 'ASTroGen.start([], node_def_class: WastroNodeDef)'

node_specialized.c:
	touch node_specialized.c

run: wastro
	./wastro examples/fib.wat fib 10
	./wastro examples/fib.wat fib 30

# --- Spec testsuite ---
#
# `make test` runs each .wast file under testsuite/ through wastro's
# --test harness, prints a per-file summary, and finishes with a
# global tally.  Files are timed out at 10s individually so that any
# pathological input doesn't stall the whole run.
#
# Requires the WebAssembly/testsuite submodule to be initialised:
#   git submodule update --init sample/wastro/testsuite

TESTSUITE = testsuite
TEST_TIMEOUT = 10

testsuite-init:
	@if [ ! -f $(TESTSUITE)/i32.wast ]; then \
		echo "Fetching testsuite submodule..."; \
		(cd ../.. && git submodule update --init sample/wastro/testsuite); \
	fi

test: wastro testsuite-init
	@p=0; f=0; sk=0; pf=0; ff=0; ef=0; \
	for w in $(TESTSUITE)/*.wast; do \
		name=$$(basename $$w .wast); \
		out=$$(timeout $(TEST_TIMEOUT) ./wastro --test $$w 2>&1); \
		last=$$(echo "$$out" | tail -1); \
		if echo "$$last" | grep -q "passed.*failed.*skipped"; then \
			pp=$$(echo "$$last" | sed -E 's/.*: ([0-9]+) passed.*/\1/'); \
			ff_=$$(echo "$$last" | sed -E 's/.* ([0-9]+) failed.*/\1/'); \
			ss=$$(echo "$$last" | sed -E 's/.* ([0-9]+) skipped.*/\1/'); \
			p=$$((p + pp)); f=$$((f + ff_)); sk=$$((sk + ss)); \
			if [ $$ff_ = 0 ]; then \
				pf=$$((pf + 1)); \
				printf "  OK  %-40s %5d pass %4d skip\n" "$$name" "$$pp" "$$ss"; \
			else \
				ff=$$((ff + 1)); \
				printf "FAIL  %-40s %5d pass %4d FAIL %4d skip\n" "$$name" "$$pp" "$$ff_" "$$ss"; \
			fi; \
		else \
			ef=$$((ef + 1)); \
			err=$$(echo "$$out" | grep -E "parse error|trap|unknown|Error" | head -1 | head -c 80); \
			printf "ERR   %-40s  %s\n" "$$name" "$$err"; \
		fi; \
	done; \
	echo "---"; \
	echo "Files:    OK=$$pf  FAIL=$$ff  ERR=$$ef  total=$$((pf + ff + ef))"; \
	echo "Asserts:  pass=$$p  fail=$$f  skip=$$sk"

# Run a single .wast (e.g. `make test-one F=i32`)
test-one: wastro testsuite-init
	./wastro --test $(TESTSUITE)/$(F).wast

# --- Benchmarks ---
#
# `make bench` runs ./bench.sh, which compares wastro at three
# optimization tiers (plain interpreter / AOT compile + run / AOT
# cached) against wasmtime on a handful of standard wasm benchmarks
# (fib, tak, sum_loop, sieve, mandelbrot, nbody, matmul, sha256).
#
# Pass `WASMTIME=...` to override the wasmtime binary path:
#   make bench WASMTIME=/usr/local/bin/wasmtime

bench: wastro
	@ruby bench.rb

clean:
	rm -f wastro $(NODE_GENFILES) node_specialized.c
	rm -rf code_store
