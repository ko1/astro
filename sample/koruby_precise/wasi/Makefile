# koruby_precise — wasm32-wasip1 build (see README.md).
#
#   make                      … build/koruby.wasm      (interpreter)
#   make aot PROG=foo.rb      … build/foo.wasm         (--build cross-compile)
#   make aot PROG=foo.rb OUT=bar.wasm
#
# AOT wasm modules come from the NATIVE koruby_precise's `--build` with
# KORUBY_BUILD_TARGET=wasi: it bakes prelude + program + SDs, emits _embed.c
# (ASTs rebuilt at startup, dispatchers pre-bound — no parse, no dlopen), and
# links everything with the wasi-sdk clang.  The `aot` target wraps that.

WASI_SDK ?= $(HOME)/wasi-sdk
WASMTIME ?= wasmtime
CC        = $(WASI_SDK)/bin/clang
AR        = $(WASI_SDK)/bin/llvm-ar

SRC     := $(abspath ..)
RUNTIME := $(abspath ../../../runtime)
BUILD   := build

# The store version must not depend on the host: the SD hashes baked by the
# emit run have to match what the linked module computes at startup.
BIGNUM_NUM = 2
GC_NUM     = 5

CPPFLAGS = -I$(SRC) -I$(SRC)/wasi -I$(SRC)/prism/include -I$(RUNTIME) \
           -include $(SRC)/wasi/wasi_decls.h \
           -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID \
           -DKORB_WASI=1 -DKORB_BIGNUM=$(BIGNUM_NUM) -DBARUBY_GC=$(GC_NUM) -DASTRO_DEBUG=0 \
           -DKORUBY_SRC_DIR='"/koruby"' -DASTRO_RUNTIME_DIR='"/koruby"' \
           -DASTRO_PRISM_INC_DIR='"/koruby"'
CFLAGS   = --target=wasm32-wasip1 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  = -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks -lm \
           -Wl,-z,stack-size=16777216

# SD translation units are self-contained (they #include node_eval.c with
# NODE_SKIP_COLD) and are generated code, so they get their own flag set.
SDFLAGS = --target=wasm32-wasip1 -O2 -w

CORE_SRCS = $(SRC)/main.c $(SRC)/node.c $(SRC)/korb_runtime.c $(SRC)/parse.c \
            $(SRC)/wasi/wasi_missing.c \
            $(RUNTIME)/precise_gc/gc_common.c $(RUNTIME)/precise_gc/gc_copy.c
CORE_OBJS = $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(CORE_SRCS))) \
            $(BUILD)/obj/prelude_blob.o

PRISM_SRCS = $(wildcard $(SRC)/prism/src/*.c) $(wildcard $(SRC)/prism/src/util/*.c)
PRISM_OBJS = $(patsubst %.c,$(BUILD)/prism/%.o,$(notdir $(PRISM_SRCS)))

.PHONY: all aot check clean clean-sd
all: $(BUILD)/koruby.wasm

$(BUILD)/koruby.wasm: $(CORE_OBJS) $(BUILD)/libprism-wasm.a $(BUILD)/libkoruby-regex-wasm.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

# --- core objects ----------------------------------------------------------
# One rule per source directory (no VPATH: the two trees have distinct names).
$(BUILD)/obj/%.o: $(SRC)/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(BUILD)/obj/%.o: $(SRC)/wasi/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
$(BUILD)/obj/%.o: $(RUNTIME)/precise_gc/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# The interpreter bakes the prelude source in (no mount needed at startup);
# main.c reads it under -DKORUBY_PRELUDE_BLOB.
$(BUILD)/obj/prelude_blob.c: $(SRC)/main.c $(wildcard $(SRC)/prelude/*.rb) \
                             $(SRC)/tools/gen_prelude_blob.rb | $(BUILD)/obj
	ruby $(SRC)/tools/gen_prelude_blob.rb $(SRC) > $@
$(BUILD)/obj/prelude_blob.o: $(BUILD)/obj/prelude_blob.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/obj/main.o: CPPFLAGS += -DKORUBY_PRELUDE_BLOB

# korb_runtime.c #includes builtins/*.c; node.c #includes the generated
# node_*.c and the runtime's astro_*.c.  List them so edits rebuild.
$(BUILD)/obj/korb_runtime.o: $(wildcard $(SRC)/builtins/*.c) $(SRC)/korb_runtime.h
$(BUILD)/obj/node.o: $(wildcard $(SRC)/node_*.c) $(SRC)/node.h $(SRC)/node_head.h \
                     $(RUNTIME)/astro_node.c $(RUNTIME)/astro_code_store.c
$(CORE_OBJS): $(SRC)/context.h Makefile

# --- regex (astrogre + bridge, statically linked) --------------------------
# wasm has no dlopen, so the koruby_regex.so / libastrogre.so pair becomes a
# static archive.  astrogre's generic ASTro symbols (ALLOC_node_* / kind_* /
# OPTION / astro_* ...) collide with koruby's, and llvm-objcopy cannot
# localize wasm symbols — so we rename them at the preprocessor level:
# pass 1 compiles unrenamed, llvm-nm lists the defined extern symbols, and
# pass 2 recompiles every TU with `#define <sym> astrogre_hidden_<sym>`
# (-include rename.h).  Renaming is uniform across the astrogre+bridge TU
# set, so cross-references resolve; only the koruby_re_* API keeps its name.
ASTROGRE := $(abspath ../../astrogre)
NM       := $(WASI_SDK)/bin/llvm-nm
REGEX_ENGINE = node.c parse.c match.c backend_astrogre.c aho_corasick.c libastrogre_option.c
REGEX_NAMES  = $(basename $(REGEX_ENGINE)) regex_bridge
RXFLAGS  = --target=wasm32-wasip1 -O2 -w -D_WASI_EMULATED_PROCESS_CLOCKS \
           -I$(ASTROGRE) -I$(RUNTIME)
RX1_OBJS = $(addprefix $(BUILD)/regex1/,$(addsuffix .o,$(REGEX_NAMES)))
RX2_OBJS = $(addprefix $(BUILD)/regex/,$(addsuffix .o,$(REGEX_NAMES)))

$(ASTROGRE)/node_eval.c: $(ASTROGRE)/node.def
	$(MAKE) -C $(ASTROGRE) node_eval.c

$(BUILD)/regex1/%.o: $(ASTROGRE)/%.c $(ASTROGRE)/node_eval.c | $(BUILD)/regex1
	$(CC) $(RXFLAGS) -c $< -o $@
$(BUILD)/regex1/regex_bridge.o: $(SRC)/regex_bridge.c | $(BUILD)/regex1
	$(CC) $(RXFLAGS) -c $< -o $@

$(BUILD)/regex/rename.h: $(RX1_OBJS) | $(BUILD)/regex
	$(NM) --extern-only --defined-only $(RX1_OBJS) \
	  | awk '$$3 != "" && $$3 !~ /^koruby_re_/ { print "#define", $$3, "astrogre_hidden_" $$3 }' \
	  | sort -u > $@

$(BUILD)/regex/%.o: $(ASTROGRE)/%.c $(BUILD)/regex/rename.h | $(BUILD)/regex
	$(CC) $(RXFLAGS) -include $(BUILD)/regex/rename.h -c $< -o $@
$(BUILD)/regex/regex_bridge.o: $(SRC)/regex_bridge.c $(BUILD)/regex/rename.h | $(BUILD)/regex
	$(CC) $(RXFLAGS) -include $(BUILD)/regex/rename.h -c $< -o $@

$(BUILD)/libkoruby-regex-wasm.a: $(RX2_OBJS)
	$(AR) rcs $@ $^

# --- prism -----------------------------------------------------------------
$(BUILD)/libprism-wasm.a: $(PRISM_OBJS)
	$(AR) rcs $@ $^
$(BUILD)/prism/%.o: $(SRC)/prism/src/%.c | $(BUILD)/prism
	$(CC) --target=wasm32-wasip1 -O2 -w -I$(SRC)/prism/include -c $< -o $@
$(BUILD)/prism/%.o: $(SRC)/prism/src/util/%.c | $(BUILD)/prism
	$(CC) --target=wasm32-wasip1 -O2 -w -I$(SRC)/prism/include -c $< -o $@

# --- AOT (native --build cross-compiling with the wasi-sdk) ----------------
# Runs from the sample root so the bake's code_store is the usual one.
aot: $(BUILD)/libprism-wasm.a $(BUILD)/libkoruby-regex-wasm.a
	@test -n "$(PROG)" || { echo "usage: make aot PROG=foo.rb [OUT=foo.wasm]" >&2; exit 2; }
	$(MAKE) -C $(SRC) koruby_precise
	cd $(SRC) && KORUBY_BUILD_TARGET=wasi WASI_SDK='$(WASI_SDK)' \
	    ./koruby_precise --build \
	    '$(abspath $(if $(OUT),$(OUT),$(BUILD)/$(notdir $(basename $(PROG))).wasm))' \
	    '$(abspath $(PROG))'

# Refresh the committed demo copy (GitHub Pages serves demo/koruby.wasm —
# build/ is gitignored).  Deliberate step, not part of `all`: the 2.3 MB
# binary should only churn in git when the demo is meant to update.
demo-wasm: $(BUILD)/koruby.wasm
	cp $(BUILD)/koruby.wasm demo/koruby.wasm
	$(WASI_SDK)/bin/llvm-strip demo/koruby.wasm
.PHONY: demo-wasm

$(BUILD)/obj $(BUILD)/prism $(BUILD)/regex1 $(BUILD)/regex:
	mkdir -p $@

clean:
	rm -rf $(BUILD)
