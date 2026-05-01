all: astrogre

NODE_GENFILES = node_eval.c node_dispatch.c node_dump.c node_specialize.c \
                node_replace.c node_alloc.c node_hash.c node_head.h

CC ?= gcc
optflags ?= -O2
PRISM_DIR = ./prism
ONIGMO_DIR = ./onigmo

CFLAGS = -I$(PRISM_DIR)/include -I. $(optflags) -ggdb3 -Wall \
         -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
         -Wno-unused-but-set-variable
LDFLAGS = -L $(PRISM_DIR)/build -Wl,-rpath=$(CURDIR)/$(PRISM_DIR)/build
LIBS = -lprism

SRCS = main.c node.c parse.c match.c selftest.c backend_astrogre.c

# Onigmo backend (built locally; opt-in via WITH_ONIGMO=1)
ifdef WITH_ONIGMO
CFLAGS += -I$(ONIGMO_DIR) -I$(ONIGMO_DIR)/enc/unicode -DUSE_ONIGMO
SRCS += backend_onigmo.c
LIBS += $(ONIGMO_DIR)/build/libonigmo.a
ASTROGRE_DEPS = $(ONIGMO_DIR)/build/libonigmo.a
endif

astrogre: $(SRCS) $(NODE_GENFILES) Makefile node.h context.h backend.h $(ASTROGRE_DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

$(NODE_GENFILES): node.def ../../lib/astrogen.rb
	ruby -I ../../lib -r astrogen -e 'ASTroGen.start []'

$(ONIGMO_DIR)/build/libonigmo.a: $(ONIGMO_DIR)/build_local.mk
	$(MAKE) -C $(ONIGMO_DIR) -f build_local.mk

clean:
	rm -fv $(NODE_GENFILES) astrogre

clean-onigmo:
	rm -rf $(ONIGMO_DIR)/build

regen:
	rm -fv $(NODE_GENFILES)
	$(MAKE)

self-test: astrogre
	./astrogre --self-test

bench: astrogre
	./astrogre --bench

.PHONY: all clean clean-onigmo regen self-test bench
