# baruby — *Barely a Ruby*

A naruby fork ([sibling sample](../naruby)) extended with **Array,
String, and Boehm GC**.  Built on the [ASTro](../../docs/idea.md)
framework — Prism for parsing, ASTroGen-driven evaluator, dlopen-based
AOT cache via the shared code store.

Designed as the **first testbed for the unified GC framework**
([`docs/gc_design.md`](../../docs/gc_design.md)).  Language surface is
deliberately tiny — fixnum / Array / String only, no OO machinery —
so the GC implementation has minimal AST root-scanning surface area
and the `value.def` DSL has only three shapes to express.

For details:
- [docs/spec.md](docs/spec.md) — what the language supports
- [docs/runtime.md](docs/runtime.md) — VALUE encoding, heap layout,
  parse-time method desugar, libgc integration
- [docs/done.md](docs/done.md) — what landed in the initial fork
- [docs/todo.md](docs/todo.md) — gaps and known limitations
- [docs/perf.md](docs/perf.md) — benchmark numbers

Naming: naruby = "**N**ot **A** ruby" → abruby = "**A B**it ruby".
baruby = "**BA**rely a ruby" sits between them: more than naruby (now
has Array + String) but still less than abruby (no class / proc /
exception / nil-vs-false / etc.).

## Install

### Prerequisites (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby ruby-bundler git libgc-dev
```

ASTroGen is plain Ruby (3.x).  baruby additionally needs **libgc**
(`libgc-dev` package on Debian/Ubuntu, ships Boehm 8.x).  No GMP /
no other external libraries.

### libprism

baruby shares naruby's prism build via a symlink (`./prism →
../naruby/prism`).  If you've already built naruby's prism, baruby
picks it up automatically.  Otherwise:

```sh
cd ../naruby
git submodule update --init prism
# (apply the small patch in ../naruby/README.md)
cd prism && bundle install && bundle exec rake
```

This produces `../naruby/prism/build/libprism.so`, which baruby's
Makefile points at via `-rpath`.

### baruby

`make` builds the host binary `./baruby`.  ASTroGen runs as part of
the build to regenerate `node_eval.c` / `node_dispatch.c` / etc. from
`node.def`.

```sh
make                  # build ./baruby
make run              # build + ./baruby test.ba.rb
make bench            # build + ruby bench/run.rb
make clean
```

If your environment can't write to ccache (common in sandbox
profiles), set `CCACHE_DISABLE=1`.

## Usage

### Run a script

```ruby
# test.ba.rb
def fib(n)
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

p fib(20)
```

```sh
./baruby --plain test.ba.rb
# 10946
# Result: 10946, node_cnt:22
# __ELAPSED__ 0.000123
```

Array + String:

```ruby
a = [1, 2, 3]
a.push(4)
p a              # [1, 2, 3, 4]
p a.size         # 4
p a[-1]          # 4

s = "hello"
p s + " " + "world"   # "hello world"
p s.size              # 5
p s[0]                # "h"
```

### GC stats

```sh
BARUBY_GC_STATS=1 ./baruby --plain bench/binary_trees.ba.rb
# ...
# __ELAPSED__ 0.931
# __GC_STATS__ alloc_bytes=336431168 heap_bytes=338685952 gc_count=12
```

Numbers come from libgc's `GC_get_total_bytes` / `GC_get_heap_size` /
`GC_get_gc_no`.

### Modes

| Flag | Mode | Notes |
|---|---|---|
| (none) | Plain + AOT bake | Run interpreted, then bake `code_store/all.so` |
| `-i` / `--plain` | Plain | No AOT load, no bake |
| `-c` | Compile only | Bake `code_store/all.so` without running the program |
| `-p` | Profile-guided | PG-bake at exit using observed `cc->body` |
| `-b` | Benchmark mode | Skip bake (timing-only) |
| `-j` | JIT | Inherited from naruby; **unwired in baruby** (TODO) |
| `--ccs` | Clear store | Wipe `code_store/` before run |

AOT / PG mode work for naruby's nodes; baruby's new nodes
(`node_ary_*` / `node_str_lit` / `node_call_*`) **have not been
verified under -c / -p yet** — see [docs/todo.md](docs/todo.md) P0.
The bench runner defaults to `--plain` for that reason.

### Benchmarks

```sh
make bench

# mode: plain, repeats: 3
# bench                       best(s)     med(s)   alloc_MB        GCs
# binary_trees                  0.93       0.94      320.8         12
# list_alloc                    1.02       1.03      763.8       1148
# string_concat                 0.97       0.98     1147.3       1706
```

Three GC-stress benches at ~1 s sustained scale; see
[docs/perf.md](docs/perf.md) for what each one exercises.

```sh
ruby bench/run.rb --mode plain -n 5      # more repeats
ruby bench/run.rb bench/binary_trees.ba.rb  # one bench only
```

## Differences vs naruby

| Aspect | naruby | baruby |
|---|---|---|
| Values | int64 only | LSB-tagged: fixnum / heap ptr / `false` |
| Heap | none | Array, String (libgc-managed) |
| GC | — | Boehm libgc (single-threaded, conservative) |
| Method calls | function calls only | `recv.method(args)` for a fixed method table; lowered at parse time to typed dispatch nodes (no class / vtable) |
| Mode coverage | plain / AOT / PG / **JIT** | plain (verified); AOT / PG / JIT not yet validated for new nodes |
| Bench focus | int loops, function calls | allocation pressure, GC throughput |

Outside of these, the framework integration (NODE layout, ASTroGen
codegen, `code_store/all.so`, hash-keyed SD lookup) is identical — see
[docs/runtime.md](docs/runtime.md) §1 for the pipeline diagram.

## Architecture in 10 lines

```
foo.ba.rb
  └─ Prism (pm_node_t)                # via ../naruby/prism (symlink)
       └─ baruby_parse.c transduce  ──► NODE * (ASTroGen format)
            │   PM_ARRAY_NODE → ary_push chain over ary_new
            │   PM_STRING_NODE → str_lit
            │   PM_CALL_NODE w/ recv + known method → call_<op>
            └─ OPTIMIZE(ast) → astro_cs_load → dlsym SD_<hash>
            └─ EVAL(c, ast)  =  (*ast->head.dispatcher)(c, ast)
       └─ build_code_store: astro_cs_compile / build / reload
```

Allocations route through libgc via macros in
[`context.h`](context.h) — every `malloc` / `calloc` / `realloc` /
`strdup` in baruby code becomes a `GC_*` call.  `main.c`'s entry
point calls `GC_INIT()` first.

The shared runtime (`../../runtime/astro_node.c`,
`../../runtime/astro_code_store.c`) is unchanged.
