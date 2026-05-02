# are — modern grep CLI on top of the astrogre regex engine.
#
# Most of the heavy lifting (parser, matcher, codegen, backends) lives
# in the parent astrogre/ directory.  This Makefile reuses those .c
# sources directly rather than building a separate static library:
# the parent's `make` already covers the codegen step (NODE_GENFILES
# from node.def via astrogen.rb), so we just need to point CC at the
# right include directories and source files.

ARE_DIR     := $(CURDIR)
ENGINE_DIR  := $(ARE_DIR)/..
RUNTIME_DIR := $(ENGINE_DIR)/../../runtime
PRISM_DIR   := $(ENGINE_DIR)/prism
ONIGMO_DIR  := $(ENGINE_DIR)/onigmo

CC      ?= gcc
optflags ?= -O2

# `verbose_mark` is wired throughout the search loop in the parent
# main.c; the unused-function suppressions match the parent build so
# we don't get noise from forked code paths that aren't reached in
# every config.
CFLAGS = -I$(PRISM_DIR)/include -I$(ENGINE_DIR) -I$(RUNTIME_DIR) -I$(ARE_DIR) \
         $(optflags) -ggdb3 -Wall -mavx2 \
         -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
         -Wno-unused-but-set-variable

LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(PRISM_DIR)/build
LIBS    = -lprism -ldl

# Source set.  The engine sources are pulled from the parent dir so
# we never fork them; only main.c + types.c live under are/.
ARE_SRCS = $(ARE_DIR)/main.c $(ARE_DIR)/types.c $(ARE_DIR)/ignore.c $(ARE_DIR)/work.c
ENG_SRCS = $(ENGINE_DIR)/node.c           \
           $(ENGINE_DIR)/parse.c          \
           $(ENGINE_DIR)/match.c          \
           $(ENGINE_DIR)/selftest.c       \
           $(ENGINE_DIR)/backend_astrogre.c \
           $(ENGINE_DIR)/aho_corasick.c

NODE_GENFILES = $(ENGINE_DIR)/node_eval.c       \
                $(ENGINE_DIR)/node_dispatch.c   \
                $(ENGINE_DIR)/node_dump.c       \
                $(ENGINE_DIR)/node_specialize.c \
                $(ENGINE_DIR)/node_replace.c    \
                $(ENGINE_DIR)/node_alloc.c      \
                $(ENGINE_DIR)/node_hash.c       \
                $(ENGINE_DIR)/node_head.h

# Optional Onigmo backend (opt-in via WITH_ONIGMO=1 — same convention
# as the parent Makefile).  When enabled, we get a `--engine=onigmo`
# alternative for benchmarks.
ifdef WITH_ONIGMO
CFLAGS  += -I$(ONIGMO_DIR) -I$(ONIGMO_DIR)/enc/unicode -DUSE_ONIGMO
ENG_SRCS += $(ENGINE_DIR)/backend_onigmo.c
LIBS    += $(ONIGMO_DIR)/build/libonigmo.a
ARE_DEPS = $(ONIGMO_DIR)/build/libonigmo.a
endif

all: are

LIBS    += -lpthread

are: $(ARE_SRCS) $(ENG_SRCS) $(NODE_GENFILES) \
     $(ARE_DIR)/types.h $(ARE_DIR)/ignore.h $(ARE_DIR)/work.h \
     $(ENGINE_DIR)/node.h $(ENGINE_DIR)/context.h $(ENGINE_DIR)/parse.h \
     $(ENGINE_DIR)/backend.h $(ENGINE_DIR)/aho_corasick.h \
     Makefile $(ARE_DEPS)
	$(CC) $(CFLAGS) -o $@ $(ARE_SRCS) $(ENG_SRCS) $(LDFLAGS) $(LIBS)

# Codegen + Onigmo: delegate to the parent Makefile so we don't
# duplicate the codegen rule (any change to node.def re-fires there).
$(NODE_GENFILES) $(ONIGMO_DIR)/build/libonigmo.a:
	$(MAKE) -C $(ENGINE_DIR) $(notdir $@)

clean:
	rm -fv are

.PHONY: all clean
