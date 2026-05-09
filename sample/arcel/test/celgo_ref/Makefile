# Build the cel-go reference evaluator (used by arcel's test harness
# and as the perf comparison target in benchmark/run.rb).
#
# We isolate Go's caches under /tmp/claude/{go,gocache,gomod} so the
# build doesn't pollute $HOME and so it works inside the project's
# sandbox (only those tmp paths are writable).  The first build pulls
# cel-go + transitive deps from proxy.golang.org (~30s); subsequent
# builds are incremental and seconds-fast.

BIN     := celgo_ref
SRC     := main.go
GOPATH  := /tmp/claude/go
GOCACHE := /tmp/claude/gocache
GOMOD   := /tmp/claude/gomod
GOENV   := GOPATH=$(GOPATH) GOCACHE=$(GOCACHE) GOMODCACHE=$(GOMOD)

all: $(BIN)

$(BIN): $(SRC) go.mod go.sum
	mkdir -p $(GOPATH) $(GOCACHE) $(GOMOD)
	$(GOENV) go build -o $(BIN) .

clean:
	rm -f $(BIN)

# Throw-away check: smoke-test the binary against a few hand-picked
# expressions so a broken build is caught immediately.
check: $(BIN)
	@./$(BIN) eval -e '1+2'                                | grep -q '^3$$' && echo "  ok  1+2 = 3"
	@./$(BIN) eval -e '"hi".size()'                        | grep -q '^2$$' && echo "  ok  size"
	@./$(BIN) eval -e '[1,2,3].all(x, x > 0)'              | grep -q '^true$$' && echo "  ok  all"
	@./$(BIN) eval -e 'x.foo' -i '{"x":{"foo":42}}'        | grep -q '^42$$' && echo "  ok  bound var"

.PHONY: all clean check
