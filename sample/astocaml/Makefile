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
	gcc -I. -I$(RUNTIME) $(optflags) $(EXTRA_CFLAGS) -ggdb3 -Wall -Wno-unused-function $(READLINE_CFLAGS) \
	    -rdynamic \
	    -o astocaml main.c node.c -ldl -lm -lgc \
	    $(READLINE_LDFLAGS) $(EXTRA_LDFLAGS)

# PGO build.  Two cycles:
#   1. astocaml binary (pgo-bin)        — instrument main.c / node.c, train, rebuild
#   2. SD .so files     (pgo-sd)        — instrument SDs through ASTRO_EXTRA_CFLAGS,
#                                          train, rebuild SDs from collected gcda
# `make pgo` runs both in sequence.  Profiles live in pgo/ and pgo-sd/.
PGO_DIR    := $(PWD)/pgo
PGO_DIR_SD := $(PWD)/pgo-sd
# method_call segfaults under -fprofile-generate (Boehm GC + the
# instrumentation runtime fight on classes); skip it from training.
PGO_TRAIN  := $(filter-out bench/method_call.ml, $(wildcard bench/*.ml))

pgo-clean:
	rm -rf $(PGO_DIR) $(PGO_DIR_SD) astocaml *.gcda code_store

# Step 1a: rebuild astocaml with -fprofile-generate
pgo-bin-gen: pgo-clean
	mkdir -p $(PGO_DIR)
	$(MAKE) astocaml EXTRA_CFLAGS="-fprofile-generate=$(PGO_DIR)" \
	                 EXTRA_LDFLAGS="-fprofile-generate=$(PGO_DIR)"

# Step 1b: train (clean code_store between runs so SDs don't carry
# stale profile-instrumented .so back into a non-instrumented binary).
pgo-bin-train: pgo-bin-gen
	@echo "==> bin training: $(PGO_TRAIN)"
	@for b in $(PGO_TRAIN); do \
	  printf "  %s\n" "$$b"; \
	  rm -rf code_store; \
	  ./astocaml -q -c "$$b" >/dev/null 2>&1 || true; \
	  ./astocaml -q -c "$$b" >/dev/null 2>&1 || true; \
	done

# Step 1c: rebuild astocaml with -fprofile-use
pgo-bin: pgo-bin-train
	rm -f astocaml
	touch main.c node.c
	$(MAKE) astocaml EXTRA_CFLAGS="-fprofile-use=$(PGO_DIR) -fprofile-correction" \
	                 EXTRA_LDFLAGS="-fprofile-use=$(PGO_DIR) -fprofile-correction"

# Step 2a: train SDs by building them with -fprofile-generate.
# astocaml is left as the post-step-1c binary (PGO-optimized).
# code_store is cleared so SDs are freshly generated under
# instrumentation; pgo-sd/ catches the gcda output.
pgo-sd-gen: pgo-bin
	mkdir -p $(PGO_DIR_SD)
	@echo "==> SD training: $(PGO_TRAIN)"
	@for b in $(PGO_TRAIN); do \
	  printf "  %s\n" "$$b"; \
	  rm -rf code_store; \
	  ASTRO_EXTRA_CFLAGS="-fprofile-generate=$(PGO_DIR_SD)" \
	  ASTRO_EXTRA_LDFLAGS="-fprofile-generate=$(PGO_DIR_SD) -lgcov" \
	    ./astocaml -q -c "$$b" >/dev/null 2>&1 || true; \
	  ASTRO_EXTRA_CFLAGS="-fprofile-generate=$(PGO_DIR_SD)" \
	  ASTRO_EXTRA_LDFLAGS="-fprofile-generate=$(PGO_DIR_SD) -lgcov" \
	    ./astocaml -q -c "$$b" >/dev/null 2>&1 || true; \
	done

# Step 2b: rebuild SDs with -fprofile-use.  Each subsequent
# `./astocaml -c <bench>` will rebuild code_store/all.so with PGO
# applied.  We pre-warm bench/*.ml so the user gets immediate wins.
pgo-sd: pgo-sd-gen
	@echo "==> SD warmup with -fprofile-use"
	@for b in $(PGO_TRAIN); do \
	  printf "  %s\n" "$$b"; \
	  rm -rf code_store; \
	  ASTRO_EXTRA_CFLAGS="-fprofile-use=$(PGO_DIR_SD) -fprofile-correction" \
	    ./astocaml -q -c "$$b" >/dev/null 2>&1 || true; \
	done

pgo: pgo-sd

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

.PHONY: test bench clean pgo pgo-clean pgo-bin-gen pgo-bin-train pgo-bin pgo-sd-gen pgo-sd
