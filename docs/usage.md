# ASTroGen Usage Guide

ASTroGen is a code generator that takes AST node definitions (`node.def`) and produces C code for an interpreter with built-in support for dispatch, hashing, dumping, allocation, and partial evaluation (specialization).

## Quick Start: Minimal Calculator

See `sample/calc/` for a complete minimal example (3 nodes, ~150 lines of user code).

Build and run:

```sh
cd sample/calc
make
./calc
```

## Architecture Overview

ASTroGen generates infrastructure code from your node definitions. You provide the node semantics; ASTroGen handles everything else.

**You write:**

| File | Purpose |
|------|---------|
| `node.def` | Node type definitions (evaluator logic) |
| `context.h` | `VALUE` type, `CTX` struct, global options |
| `node.h` | Type declarations, `EVAL`/`DUMP`/`OPTIMIZE` function signatures |
| `node.c` | Runtime support (hash functions, node allocation, code repository, `INIT`/`EVAL`/`DUMP`/`OPTIMIZE` implementations) |
| `main.c` | Parser, entry point, AST construction |

**ASTroGen generates:**

| File | Contents |
|------|----------|
| `node_head.h` | Function pointer typedefs, `NodeKind` struct, node struct definitions, `Node` union, allocator declarations |
| `node_eval.c` | `EVAL_xxx()` — unwraps node fields and calls your evaluator |
| `node_dispatch.c` | `DISPATCH_xxx()` — dispatcher wrappers (the specialization target) |
| `node_alloc.c` | `ALLOC_xxx()` — node allocators + `NodeKind` tables |
| `node_hash.c` | `HASH_xxx()` — Merkle tree hash for each node type |
| `node_dump.c` | `DUMP_xxx()` — AST pretty-printer |
| `node_specialize.c` | `SPECIALIZE_xxx()` — generates specialized C code |
| `node_replace.c` | `REPLACER_xxx()` — replaces a child node with another node |

## Writing `node.def`

Each node is defined with `NODE_DEF` followed by a function signature and body:

```c
NODE_DEF
node_num(CTX *c, NODE *n, int32_t num)
{
    return num;
}

NODE_DEF
node_add(CTX *c, NODE *n, NODE *lv, NODE *rv)
{
    return EVAL_ARG(c, lv) + EVAL_ARG(c, rv);
}
```

### Signature rules

- First two parameters are always `CTX *c, NODE *n` (context and self node).
- Remaining parameters are **operands** stored in the node struct.
- `NODE *` operands are child nodes. ASTroGen passes their dispatcher alongside them for direct dispatch.
- Use `EVAL_ARG(c, child)` to evaluate child nodes (calls the child's dispatcher directly).

### Supported operand types (base ASTroGen)

| Type | Hash | Dump | Description |
|------|------|------|-------------|
| `int32_t` | MurmurHash | `%d` | Signed 32-bit integer |
| `uint32_t` | MurmurHash | `%u` | Unsigned 32-bit integer |
| `NODE *` | Recursive node hash | Recursive dump | Child AST node |
| `const char *` | FNV hash | `"%s"` | String literal |

Additional operand types can be added by subclassing (see [Extending ASTroGen](#extending-astrogen)).

### Node options

Add options after `NODE_DEF`:

```c
NODE_DEF @noinline
node_def(CTX *c, NODE *n, const char *name, NODE *body, uint32_t params_cnt)
{
    // This node will not be inlined during specialization.
}
```

- `@noinline` — prevents specialization from inlining this node's dispatched calls.

Operand-level annotation:

- `<type> <name>@ref` — store the operand by reference rather than embedding its value in the node struct. Use this for mutable side data such as inline method caches that should be shared across specialized copies. `@ref` operands are skipped by the hash function.

## Writing `context.h`

Define the value type, execution context, and global options:

```c
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef int64_t VALUE;

typedef struct {
    int dummy;  // minimal context
} CTX;

struct my_option {
    bool record_all;
    bool no_compiled_code;
    // ...
};

extern struct my_option OPTION;
```

For a language with variables and functions, `CTX` typically contains a value stack and frame pointer:

```c
typedef struct {
    VALUE *fp;              // frame pointer into value stack
    // ... function table, etc.
} CTX;
```

## Writing `node.h`

Declare types and functions that both your code and generated code use.

Note: `node_head.h` (generated) provides function pointer typedefs (`node_hash_func_t`, `node_specializer_func_t`, `node_dumper_func_t`, `node_replacer_func_t`), the `NodeKind` struct, node struct definitions, and `ALLOC` declarations. You only need to define the base types (`NODE`, `node_dispatcher_func_t`, `node_hash_t`) and `NodeHead` before `#include "node_head.h"`.

```c
#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
VALUE EVAL(CTX *c, NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) \
    (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

NODE *code_repo_find(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    // Required by generated allocators:
    enum jit_status {
        JIT_STATUS_Unknown,
        // JIT_STATUS_Querying, JIT_STATUS_NotFound, etc. (if using JIT)
    } jit_status;
    unsigned int dispatch_cnt;
};

// node_head.h provides:
//   - Function pointer typedefs (node_hash_func_t, node_specializer_func_t, etc.)
//   - struct NodeKind { ... };
//   - Node struct definitions and Node union
//   - ALLOC_xxx declarations
#include "node_head.h"
```

## Writing `node.c`

You provide a few small helpers (allocator, optional dispatch trace) and
then `#include` two runtime files plus the generated ones.  The runtime
files (`runtime/astro_node.c` and `runtime/astro_code_store.c`) are
intentionally `#include`-style — they need to see your `NODE`,
`node_hash_t`, and `NodeKind` types, so they can't be a separately-
compiled library.

```c
#include "node.h"

// --- Required by runtime/astro_node.c (#include'd below) ---
static NODE *node_allocate(size_t size) {
    NODE *n = calloc(1, size);
    if (!n) { fprintf(stderr, "out of memory\n"); abort(); }
    return n;
}

static void dispatch_info(CTX *c, NODE *n, bool end) {
    // Optional tracing hook, called from generated DISPATCH_xxx when
    // a debug option is set.  Empty body is fine if you don't need it.
}

// --- Common runtime helpers ---
// Provides hash_merge / hash_cstr / hash_uint32 / hash_node, plus the
// public HASH() / DUMP() implementations and alloc_dispatcher_name().
// Must come AFTER node_allocate / dispatch_info above.
#include "astro_node.c"

// --- Code Store (AOT / PG / JIT lookup + build) ---
// Provides astro_cs_init / astro_cs_load / astro_cs_compile /
// astro_cs_build / astro_cs_reload / astro_cs_disasm.
#include "astro_code_store.c"

// --- Generated code ---
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// --- The two functions that ASTroGen / runtime expect you to provide ---

VALUE EVAL(CTX *c, NODE *n) {
    return (*n->head.dispatcher)(c, n);
}

NODE *OPTIMIZE(NODE *n) {
    // Default: ask the code store; on hit, the dispatcher is patched in
    // place and subsequent EVAL goes straight to the specialised SD.
    astro_cs_load(n, NULL);
    return n;
}

void INIT(void) {
    // Initialise the code store.  store_dir is where SD_<hash>.c and
    // all.so live; src_dir is what specialised .c files will #include
    // (your node.h etc.); the version arg is a cache-busting key — pass
    // 0 to skip version checking.
    astro_cs_init("code_store", ".", 0);
}
```

Larger samples customise `OPTIMIZE` and `INIT` — e.g. `sample/naruby`
launches a JIT submit thread inside `OPTIMIZE`, `sample/astrogre`
collects all entry nodes during parse and walks the list in
`astrogre_pattern_aot_compile` (see [Entry nodes](#entry-nodes) below).

## Build Setup

### Makefile

The build only needs the host binary itself; specialised code is
generated and compiled at runtime by `astro_cs_build`.  Point `-I` at
both your sample directory and `runtime/` so `node.c` can find
`astro_node.c` and `astro_code_store.c` for `#include`.

```makefile
ASTRO_LIB     = path/to/astro/lib
ASTRO_RUNTIME = path/to/astro/runtime

GENERATED = node_eval.c node_dispatch.c node_hash.c \
            node_dump.c node_alloc.c node_specialize.c \
            node_replace.c node_head.h

# Generate from node.def
$(GENERATED): node.def
	ruby -I $(ASTRO_LIB) -r astrogen -e 'ASTroGen.start []'

my_lang: main.c node.c $(GENERATED)
	gcc -O2 -I. -I$(ASTRO_RUNTIME) -o my_lang main.c node.c -ldl
```

`-ldl` is needed because `astro_code_store.c` calls `dlopen` /
`dlsym` to load `code_store/all.so` at runtime.

### Code Store lifecycle

The runtime code store replaces the older two-pass node_specialized.c
flow.  Specialised dispatchers are emitted, compiled, and patched into
the AST at runtime — no second `gcc` invocation on the host source.

```
[1st run, no code_store/all.so present]
  INIT() → astro_cs_init("code_store", ".", 0)   # nothing to load yet
  parse → ALLOC nodes → OPTIMIZE → astro_cs_load # all miss
  EVAL                                           # interpreter path
  astro_cs_compile(entry, NULL)                  # writes code_store/c/SD_<hash>.c
  astro_cs_build(NULL)                           # invokes cc to produce all.so
  astro_cs_reload()                              # dlopen the just-built .so

[2nd run, all.so present]
  INIT() → astro_cs_init(...)                    # dlopen all.so
  parse → ALLOC → OPTIMIZE → astro_cs_load       # hit → dispatcher patched
  EVAL                                           # specialised SD path
```

For AOT mode, samples typically call `astro_cs_compile` →
`astro_cs_build` → `astro_cs_reload` once during the first run, then
re-resolve every NODE's dispatcher so the freshly-baked SDs take
effect immediately (otherwise only the next process invocation
benefits).  See `sample/<lang>/node.c::INIT` and the
`<lang>_reload_all_dispatchers` helper for the standard pattern.

For JIT mode, `OPTIMIZE` submits the entry node to a background
thread that runs `astro_cs_compile` + `astro_cs_build` asynchronously,
then patches the dispatcher when the build completes.  See
`sample/naruby/`.

## Runtime Library — `runtime/astro_node.c` and `runtime/astro_code_store.{h,c}`

The runtime files are framework-supplied helpers that every ASTro
sample pulls in.  They are intentionally `#include`-style (not a
separately compiled library) because they need to see the host's
`NODE`, `node_hash_t`, `NodeKind` types — which are defined per
sample.  Pre-conditions for each include are noted below.

### `runtime/astro_node.c` — common per-node helpers

`#include` it from your `node.c` AFTER defining `node_allocate` and
`dispatch_info`, AFTER `#include "node.h"`, and BEFORE
`#include "astro_code_store.c"` and the generated files.

| Symbol | Purpose |
|--------|---------|
| `node_hash_t hash_merge(h, v)`               | Combine two hashes (FNV-mix variant). |
| `node_hash_t hash_cstr(const char *s)`        | FNV-1a 64-bit string hash. |
| `node_hash_t hash_uint32(uint32_t)`           | MurmurHash3 finaliser. |
| `node_hash_t hash_uint64(uint64_t)`           | Same, on 64-bit. |
| `node_hash_t hash_double(double)`             | Same, on `double` bit-pattern. |
| `node_hash_t hash_node(NODE *)`               | Cached child-node hash; uses `head.flags.has_hash_value`. |
| `node_hash_t HASH(NODE *)`                    | Public hash entry; cycles → 0; cached on first call. |
| `void DUMP(FILE *, NODE *, bool oneline)`     | Public dump entry; cycle-safe via `flags.is_dumping`. |
| `astro_fprintf_cstr(FILE *, const char *)`   | Escape-and-quote a C string for embedding in generated source. |
| `alloc_dispatcher_name(NODE *)`               | Produce `SD_<hash>` (or `PGSD_<hash>` under PG mode). |

The hash functions are the building blocks ASTroGen's generated
`node_hash.c` calls into — you don't write them yourself, you just
have to provide the file via the include.

### `runtime/astro_code_store.h` — public AOT/PG/JIT API

Include this header before `astro_code_store.c` so your sample's
public-facing code (e.g. `INIT`, `OPTIMIZE`) can call into it.

```c
// Initialise.  Reads `store_dir`/all.so if present, dlopens it.  Subsequent
// astro_cs_load calls hit the in-memory dispatcher table populated from .so
// symbols.  src_dir is what generated SD_<hash>.c will #include — usually
// the directory containing your node.h.  version is a cache-busting key
// (e.g. mtime of host binary); 0 disables the check.
void astro_cs_init(const char *store_dir, const char *src_dir, uint64_t version);

// Look up specialised code for n's hash; on hit, patches n->head.dispatcher
// and returns true.  `file` (nullable) selects PGC mode (Hopt lookup) when
// non-NULL; pass NULL for AOT (Horg) lookup.
bool astro_cs_load(NODE *n, const char *file);

// Generate specialised C source for an entry node.  The entry becomes the
// public extern symbol; its subtree's nodes are emitted as static inline
// children — gcc inlines them through the dispatcher arg, so the chain
// becomes one tight basic block in the SD function.  Writes to
// <store_dir>/c/SD_<hash>.c (or PGSD_<hash>.c if `file != NULL`).
void astro_cs_compile(NODE *entry, const char *file);

// Compile every SD_*.c in store_dir into all.so via `make -j`.  extra_cflags
// is appended to the cc invocation (e.g. `-I/usr/include/ruby` for embedded
// hosts); pass NULL when none.
void astro_cs_build(const char *extra_cflags);

// dlclose + dlopen of all.so.  Use after astro_cs_build to make freshly-
// baked symbols visible in the running process.
void astro_cs_reload(void);

// Disassembly print (via objdump) of the specialised dispatcher for n.
// No-op if n isn't specialised yet.  Diagnostic only.
void astro_cs_disasm(NODE *n);
```

The full design rationale (entry-as-public, hash-keyed dedup, the
PGC vs AOT split, the JIT/AOT/PG mode matrix) is in
[`idea.md` §7](./idea.md).  Known traps with the dlopen-based loader
(path-name caching, mid-run library replacement, etc.) are in
[`code_store_quirks.md`](./code_store_quirks.md).

### Entry nodes — what to register

Most samples have a single entry per top-level execution unit (a
script, a method body, a regex pattern), and a single
`astro_cs_compile(root, NULL)` call covers the whole tree because
every reachable node gets emitted as `static inline` inside the
root's SD file.  The chain of dispatcher pointers passed via
`EVAL_ARG`-style arguments lets gcc inline the entire chain.

But some patterns of dispatch break that assumption: when a NODE's
dispatcher is read at runtime through some indirection — a CTX
field, a stack frame, a runtime selection — the SD specialiser
cannot constant-fold the dispatcher value.  Those sites need the
target NODE registered as its **own** entry so its SD becomes a
public extern symbol that `astro_cs_load` can dlsym.

Concrete examples from `sample/astrogre`:

| Site (in `node.def`)                       | Entry NODE that needs registration |
|--------------------------------------------|-----------------------------------|
| `EVAL(c, c->rep_cont_sentinel)` — global singleton | The rep_cont sentinel itself      |
| `EVAL(c, f->body)` — frame-stored          | Each `node_re_rep`'s body operand |
| `EVAL(c, f->outer_next)` — frame-stored    | Each `node_re_rep`'s outer_next   |
| `EVAL(c, c->sub_chains[idx])` — array lookup | Each subroutine chain root      |
| `EVAL(c, c->sub_top->return_to)` — frame-stored | Each subroutine_call's outer_next |

The convention used by `astrogre` is to push these candidates onto
a list during IR-lower (when the role is locally known: "this is
a rep body", "this is a subroutine target") and iterate the list
in the AOT-compile entry point.  See `sample/astrogre/parse.c::lower_push_entry`
and `astrogre_pattern_aot_compile`.

A previous approach was a post-build file rewrite that turned every
inner SD into an extern weak wrapper (`SD_<hash>` → `SD_<hash>_INL`
+ extern alias).  It worked but was a band-aid — entry registration
is the framework-supported path and yields cleaner dlopen output.

For samples without frame-stored or runtime-indirect dispatch (e.g.
`sample/calc`, `sample/naruby` for the simple cases), one entry per
top-level callable (= per method body, per script root) is enough.

## Extending ASTroGen

ASTroGen is designed to be extended via subclassing. The class hierarchy is:

```
ASTroGen (module)
  NodeDef
    Node
      Operand
```

Each level uses `self.class::ClassName` for dispatch, so subclasses are automatically picked up.

### Adding custom generation tasks

Use `register_gen_task` to add new per-node code generation tasks. Each task generates a `node_<name>.c` file and optionally adds a function pointer typedef and a field to `NodeKind`.

For example, `sample/abruby/` adds a GC mark function generator:

```ruby
# abruby_gen.rb
require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    def build_marker
      node_ops = @operands.reject(&:ref?).select(&:node?)
      marks = node_ops.map { |op| "    MARK(n->u.#{@name}.#{op.name});" }
      <<~C
      static void
      MARKER_#{@name}(NODE *n)
      {
      #{marks.join("\n")}
      }
      C
    end
  end

  def build_mark
    <<~C
    // This file is auto-generated from #{@file}.
    // GC mark functions
    #{@nodes.map{|name, n| n.build_marker}.join("\n")}
    C
  end
end
```

`register_gen_task` options:

| Option | Description |
|--------|-------------|
| `func_typedef:` | C typedef for the function pointer (added to `node_head.h`) |
| `func_prefix:` | Prefix for per-node functions (e.g., `"MARKER_"` → `MARKER_node_if`) |
| `kind_field:` | Field declaration for `NodeKind` struct (e.g., `"node_marker_func_t marker"`) |
| `generate_file:` | Whether to generate `node_<name>.c` (default: `true`) |

The generator looks for a `build_<name>` method on `NodeDef` to produce the file contents, and a `build_<name>` or `build_marker` (etc.) method on `Node` for per-node code.

### Adding custom operand types

Create a subclass of `NodeDef` with a nested `Node::Operand` that handles your types:

```ruby
# my_lang_gen.rb
require 'astrogen'

class MyLangNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val)
        case @type
        when 'struct my_cache *'
          '0'  # not hashable
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct my_cache *'
          "        fprintf(fp, \"<cache>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct my_cache *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
```

Invoke with:

```makefile
ASTROGEN = ruby -I path/to/astro/lib -r ./my_lang_gen \
               -e 'ASTroGen.start([], node_def_class: MyLangNodeDef)'

$(GENERATED): node.def path/to/astro/lib/astrogen.rb my_lang_gen.rb
	$(ASTROGEN)
```

### Overriding `result_type`

By default, all generated eval/dispatch functions return `VALUE`. Override `result_type` in a `Node` subclass to change this:

```ruby
class MyLangNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"
  end
end
```

This changes all generated function signatures and local variable declarations from `VALUE` to `RESULT`.

### Overriding node code generation

For deeper customization (e.g., custom allocators), override methods on `Node`:

```ruby
class MyLangNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def build_allocator
      # Custom allocator using TypedData_Wrap_Struct, arena allocation, etc.
    end
  end
end
```

## Reference

### Key macros and functions in node.def

| Name | Description |
|------|-------------|
| `EVAL_ARG(c, child)` | Evaluate a child node via the dispatcher value passed in as a NODE_DEF parameter.  The SD specialiser folds this into a direct call to the child's inlined SD, so the chain becomes one tight basic block. |
| `EVAL(c, n)` | Evaluate a node via runtime read of `n->head.dispatcher`.  Use when the NODE pointer comes from a CTX field, stack frame, or runtime selection — anywhere the specialiser cannot constant-fold the dispatcher value (e.g. singletons, frame-stored continuations, array lookups).  The target NODE typically needs to be registered with `astro_cs_compile` so it has a public-extern SD that dlsym can find. |
| `OPTIMIZE(n)` | Walk the tree and ask `astro_cs_load` to patch each node's dispatcher to its specialised SD when the code store has one. |
| `OPTION` | Global options struct (user-defined). |

### Runtime API summary

| Function | Header | Purpose |
|----------|--------|---------|
| `astro_cs_init(store_dir, src_dir, ver)` | `astro_code_store.h` | Set up code-store paths; dlopen all.so if present. |
| `astro_cs_load(n, file)`                  | "                    | Try patching n's dispatcher from the dlopen'd SDs. |
| `astro_cs_compile(entry, file)`           | "                    | Emit specialised C source for an entry node's subtree. |
| `astro_cs_build(extra_cflags)`            | "                    | `make -j` all SD_*.c → all.so. |
| `astro_cs_reload()`                       | "                    | dlclose + dlopen of all.so to pick up freshly-baked code. |
| `astro_cs_disasm(n)`                      | "                    | Diagnostic: objdump n's specialised SD. |
| `HASH(n)`                                 | (via `astro_node.c`) | Cached structural hash, used as the SD lookup key. |
| `DUMP(fp, n, oneline)`                    | "                    | Cycle-safe AST pretty-printer. |
| `EVAL(c, n)` / `EVAL_ARG(c, n)`           | (defined per-sample) | Dispatch macros — see the table above. |

### ASTroGen command-line options

```
ruby -r astrogen -e 'ASTroGen.start ARGV' -- [options]
  --input=FILE          Input node.def file (default: node.def)
  --output-prefix=STR   Output file prefix (default: node)
  --output-head=FILE    Header output file (default: node_head.h)
  --output-dir=DIR      Output directory (default: current directory)
  --verbose             Enable verbose output
```

### Examples

- `sample/calc/` — Minimal calculator (3 nodes).  Good starting point for understanding the runtime + ASTroGen flow without language-design noise.
- `sample/naruby/` — Ruby subset with functions, variables, operators, JIT support.  Standalone C program; the canonical "real language" example.
- `sample/abruby/` — Ruby subset as a CRuby C extension.  Classes, methods, blocks, GC integration, builtins.  Demonstrates `register_gen_task` for custom mark function generation and `@ref` operands for inline caches.
- `sample/luastro/`, `sample/ascheme/`, `sample/wastro/`, `sample/jstro/`, `sample/astocaml/`, `sample/castro/`, `sample/koruby/`, `sample/asom/` — additional language fronts using the same framework.
- `sample/astrogre/` — Ruby/Onigmo-compatible regex engine with a grep CLI (`are`).  Demonstrates entry-node registration for AST shapes that have multiple runtime-indirect dispatch sites (rep continuations, subroutine chains).  Particularly useful when the AST isn't tree-shaped from a single root.
