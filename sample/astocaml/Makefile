SHELL := /bin/bash

NODE_GENFILES=node_eval.c \
              node_dispatch.c \
              node_dump.c \
              node_specialize.c \
              node_replace.c \
              node_alloc.c \
              node_hash.c \
              node_head.h

RUNTIME=../../runtime

optflags=-O2

READLINE_CFLAGS := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

astocaml: main.c node.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -o astocaml main.c node.c -ldl -lm \
	    $(READLINE_LDFLAGS)

node_eval.c: ../../lib/* node.def astocaml_gen.rb
	ruby -I ../../lib -I . -r astrogen -r astocaml_gen \
	    -e 'ASTroGen.start([], node_def_class: AstocamlNodeDef)'

# --- Tests ---
#
# Each test/*.ml is run; stdout is compared to test/*.expected.

TESTS = $(wildcard test/*.ml)

test: astocaml
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.ml}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(./astocaml -q "$$t" 2>&1); \
	  want=$$(cat "$$exp"); \
	  if [ "$$out" = "$$want" ]; then \
	    p=$$((p+1)); printf "  OK  %s\n" "$$t"; \
	  else \
	    f=$$((f+1)); \
	    printf "FAIL  %s\n" "$$t"; \
	    diff <(printf "%s" "$$want") <(printf "%s" "$$out") | head -20 | sed 's/^/      /'; \
	  fi; \
	done; \
	echo "---"; \
	echo "passed=$$p  failed=$$f  total=$$((p+f))"; \
	test $$f -eq 0

bench: astocaml
	@for b in bench/*.ml; do \
	  printf "==> %s\n" "$$b"; \
	  /usr/bin/time -f "  %e s, %M KB" ./astocaml -q "$$b" 2>&1 | tail -3; \
	done

clean:
	rm -f astocaml $(NODE_GENFILES) node_specialized.c
	rm -rf code_store

.PHONY: test bench clean
