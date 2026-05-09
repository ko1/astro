# Build celcpp_bench against an out-of-tree cel-cpp checkout.
#
# cel-cpp uses Bazel + protobuf + Abseil + RE2 + ANTLR which together
# pull >1 GB of build artefacts; we deliberately keep the heavy build
# out of the arcel repo and just shuttle our `.cc` + `BUILD` into it.
#
# Usage:
#   make CEL_CPP_DIR=/path/to/cel-cpp
#
# `make celgo` (../celgo_ref/Makefile) handles cel-go similarly.

BIN          := celcpp_bench
CEL_CPP_DIR  ?= /tmp/celcpp/cel-cpp
USE_BAZEL    ?= 7.3.2

# Bazel needs a writable HOME / cache outside of the project's read-only
# sandboxed paths.  We park them under /tmp/claude/* so the build is
# reproducible across runs and the project tree stays clean.
TEST_TMPDIR  ?= /tmp/claude/bazel_tmp
BAZEL_HOME   ?= /tmp/claude/bazel_home
CCACHE_DIR   ?= /tmp/claude/ccache_d

all: $(BIN)

$(BIN): celcpp_bench.cc BUILD $(CEL_CPP_DIR)
	@if [ ! -d "$(CEL_CPP_DIR)" ]; then \
	  echo "celcpp_bench: $(CEL_CPP_DIR) not found."; \
	  echo "  $$ git clone https://github.com/google/cel-cpp.git $(CEL_CPP_DIR)"; \
	  exit 1; \
	fi
	mkdir -p $(CEL_CPP_DIR)/celcpp_bench
	cp celcpp_bench.cc BUILD $(CEL_CPP_DIR)/celcpp_bench/
	mkdir -p $(TEST_TMPDIR) $(BAZEL_HOME) $(CCACHE_DIR)
	cd $(CEL_CPP_DIR) && \
	  USE_BAZEL_VERSION=$(USE_BAZEL) \
	  TEST_TMPDIR=$(TEST_TMPDIR) HOME=$(BAZEL_HOME) \
	  CC=/usr/bin/gcc CXX=/usr/bin/g++ \
	  bazelisk build --spawn_strategy=local \
	    --action_env=PATH=/usr/bin:/bin \
	    --action_env=CC=/usr/bin/gcc --action_env=CXX=/usr/bin/g++ \
	    -c opt //celcpp_bench:celcpp_bench
	cp $(CEL_CPP_DIR)/bazel-bin/celcpp_bench/celcpp_bench ./$(BIN)

clean:
	rm -f $(BIN)

# Smoke check that the binary works end-to-end before benchmarking.
check: $(BIN)
	@./$(BIN) -e '1+2' -n 100 | awk '{ if ($$3 > 0) print "  ok  smoke"; else exit 1 }'

.PHONY: all clean check
