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
| `node_head.h` | Struct definitions for each node type, `Node` union, allocator declarations |
| `node_eval.c` | `EVAL_xxx()` — unwraps node fields and calls your evaluator |
| `node_dispatch.c` | `DISPATCH_xxx()` — dispatcher wrappers (the specialization target) |
| `node_alloc.c` | `ALLOC_xxx()` — node allocators + `NodeKind` tables |
| `node_hash.c` | `HASH_xxx()` — Merkle tree hash for each node type |
| `node_dump.c` | `DUMP_xxx()` — AST pretty-printer |
| `node_specialize.c` | `SPECIALIZE_xxx()` — generates specialized C code |

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

Declare types and functions that both your code and generated code use:

```c
#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;
typedef node_hash_t (*node_hash_func_t)(NODE *n);
typedef void (*node_specializer_func_t)(FILE *f, NODE *n);
typedef void (*node_dumper_func_t)(FILE *f, NODE *n, bool oneline);

void INIT(void);
node_hash_t HASH(NODE *n);
VALUE EVAL(CTX *c, NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) \
    (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

NODE *code_repo_fnid(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

struct NodeKind {
    const char *default_dispatcher_name;
    node_dispatcher_func_t default_dispatcher;
    node_hash_func_t hash_func;
    node_specializer_func_t specializer;
    node_dumper_func_t dumper;
};

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
};

#include "node_head.h"  // generated: node structs, Node union, ALLOC declarations
```

## Writing `node.c`

Provide the runtime support functions, then `#include` the generated files:

```c
#include "node.h"

// --- Hash functions (required by generated hash code) ---
static node_hash_t hash_merge(node_hash_t h, node_hash_t v) { /* ... */ }
static node_hash_t hash_cstr(const char *s) { /* FNV hash */ }
static node_hash_t hash_uint32(uint32_t ui) { /* MurmurHash */ }
static node_hash_t hash_node(NODE *n) { /* cached HASH() call */ }

// --- Node allocation ---
static NODE *node_allocate(size_t size) { /* malloc + error check */ }

// --- Debug tracing ---
static void dispatch_info(CTX *c, NODE *n, bool end) { /* optional */ }

// --- Specialized code repository ---
// sc_repo_clear, sc_repo_search, sc_repo_add, alloc_dispatcher_name
// ... (see sample/calc/node.c for reference)

// --- Core functions: HASH, EVAL, DUMP, OPTIMIZE, SPECIALIZE, INIT ---
// ... (see sample/calc/node.c for reference)

// --- Include generated code ---
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_alloc.c"
#include "node_specialized.c"

#ifndef NODE_SPECIALIZED_INCLUDED
static struct specialized_code sc_entries[] = {};
#endif

void INIT(void) {
    // Load pre-compiled specialized dispatchers from sc_entries[]
}
```

## Build Setup

### Makefile

```makefile
GENERATED = node_eval.c node_dispatch.c node_hash.c \
            node_dump.c node_alloc.c node_specialize.c node_head.h

# Generate from node.def
$(GENERATED): node.def
	ruby -I path/to/astro/lib -r astrogen -e 'ASTroGen.start []'

# Empty file on first build (populated after first run)
node_specialized.c:
	touch node_specialized.c

my_lang: main.c node.c $(GENERATED)
	gcc -O3 -o my_lang main.c node.c
```

### Two-pass compilation cycle

1. **First build**: `node_specialized.c` is empty. The interpreter runs and generates specialized dispatchers into `node_specialized.c`.
2. **Second build**: Recompile with the generated specializations. The C compiler inlines the specialized dispatchers, producing optimized native code.

## Extending ASTroGen

ASTroGen is designed to be extended via subclassing. The class hierarchy is:

```
ASTroGen (module)
  NodeDef
    Node
      Operand
```

Each level uses `self.class::ClassName` for dispatch, so subclasses are automatically picked up.

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
$(GENERATED): node.def my_lang_gen.rb
	ruby -I path/to/astro/lib -r ./my_lang_gen \
	     -e 'ASTroGen.start([], node_def_class: MyLangNodeDef)'
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
| `EVAL_ARG(c, child)` | Evaluate a child node (direct dispatcher call) |
| `OPTIMIZE(n)` | Look up and apply specialized dispatcher for node `n` |
| `OPTION` | Global options struct (user-defined) |

### ASTroGen command-line options

```
ruby -r astrogen -e 'ASTroGen.start ARGV' -- [options]
  --input=FILE          Input node.def file (default: node.def)
  --output-prefix=STR   Output file prefix (default: node)
  --output-head=FILE    Header output file (default: node_head.h)
  --verbose             Enable verbose output
```

### Examples

- `sample/calc/` — Minimal calculator (3 nodes). Good starting point.
- `sample/naruby/` — Ruby subset with functions, variables, operators, JIT support.
- `sample/abruby/` — Ruby subset as a CRuby C extension with classes, methods, GC integration.
