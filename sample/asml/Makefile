SHELL := /bin/bash

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

READLINE_CFLAGS  := $(shell echo 'int main(){return 0;}' | gcc -x c -lreadline - -o /dev/null 2>/dev/null && echo "-DUSE_READLINE")
READLINE_LDFLAGS := $(if $(READLINE_CFLAGS),-lreadline)

asml: main.c node.c Makefile $(NODE_GENFILES) context.h node.h
	gcc -I. -I$(RUNTIME) $(optflags) $(EXTRA_CFLAGS) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -o asml main.c node.c -ldl -lm \
	    $(READLINE_LDFLAGS) $(EXTRA_LDFLAGS)

node_eval.c: ../../lib/* node.def asml_gen.rb
	ruby -I ../../lib -I . -r astrogen -r asml_gen \
	    -e 'ASTroGen.start([], node_def_class: AsmlNodeDef)'

# --- Tests ---

TESTS = $(filter-out test/typeerror.sml,$(wildcard test/*.sml))

test: asml
	@p=0; f=0; \
	for t in $(TESTS); do \
	  exp=$${t%.sml}.expected; \
	  if [ ! -f $$exp ]; then \
	    printf "SKIP  %s  (no .expected)\n" "$$t"; continue; \
	  fi; \
	  out=$$(./asml -q "$$t" 2>&1); \
	  want=$$(cat "$$exp"); \
	  if [ "$$out" = "$$want" ]; then \
	    p=$$((p+1)); printf "  OK  %s\n" "$$t"; \
	  else \
	    f=$$((f+1)); \
	    printf "FAIL  %s\n" "$$t"; \
	    diff <(printf "%s" "$$want") <(printf "%s" "$$out") | head -20 | sed 's/^/      /'; \
	  fi; \
	done; \
	if ./asml -q test/typeerror.sml 2>/dev/null; then \
	  f=$$((f+1)); printf "FAIL  test/typeerror.sml  (should have been rejected)\n"; \
	else \
	  p=$$((p+1)); printf "  OK  test/typeerror.sml  (rejected as expected)\n"; \
	fi; \
	echo "---"; \
	echo "passed=$$p  failed=$$f  total=$$((p+f))"; \
	test $$f -eq 0

clean:
	rm -f asml $(NODE_GENFILES) node_specialized.c
	rm -rf code_store

.PHONY: test clean
