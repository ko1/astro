# naruby — *Not a Ruby*

A minimal Ruby subset built on the [ASTro](../../docs/idea.md)
framework.  Prism for parsing, ASTroGen-driven evaluator, dlopen-based
AOT cache via the shared code store, optional out-of-process JIT.

Designed as the evaluation target for the ASTro paper (VMIL 2025 / PPL
2026): **the only sample that exercises all four execution modes —
plain, AOT, profile-guided AOT, and JIT — from a single binary.**
Language surface is intentionally tiny (signed-integer values only,
no objects/strings/blocks).

For details:
- [docs/spec.md](docs/spec.md) — what the language supports
- [docs/done.md](docs/done.md) — implemented nodes / features / modes
- [docs/todo.md](docs/todo.md) — gaps and known limitations
- [docs/runtime.md](docs/runtime.md) — pipeline, NODE layout, 4-mode
  switching, ASTroGen / runtime/ integration
- [docs/perf.md](docs/perf.md) — benchmark numbers and observations

ASTro paper: see [../../docs/idea.md](../../docs/idea.md) §6 for the
naruby evaluation, §5 for the L0/L1/L2 JIT tier description.

## Build

### libprism

The Prism submodule needs one small patch so naruby's parser can read
the constant pool:

```sh
git submodule update --init prism
cd prism
patch -p1 <<'EOF'
diff --git a/include/prism/util/pm_constant_pool.h b/include/prism/util/pm_constant_pool.h
--- a/include/prism/util/pm_constant_pool.h
+++ b/include/prism/util/pm_constant_pool.h
@@ -161,7 +161,7 @@ bool pm_constant_pool_init(pm_constant_pool_t *pool, uint32_t capacity);
  * @param constant_id The id of the constant to get.
  * @return A pointer to the constant.
  */
-pm_constant_t * pm_constant_pool_id_to_constant(const pm_constant_pool_t *pool, pm_constant_id_t constant_id);
+PRISM_EXPORTED_FUNCTION pm_constant_t * pm_constant_pool_id_to_constant(const pm_constant_pool_t *pool, pm_constant_id_t constant_id);

 /**
  * Find a constant in a constant pool. Returns the id of the constant, or 0 if
EOF
bundle install
bundle exec rake
```

This produces `prism/build/libprism.so`.

### naruby

`make` builds the host binary `./naruby`.  ASTroGen runs as part of the
build to regenerate `node_eval.c` / `node_dispatch.c` / etc. from
`node.def`.

```sh
make                 # build ./naruby
make run             # build + ./naruby test.na.rb
make c               # build + ./naruby -c test.na.rb (compile-only)
make bench BITEM=fib # ruby / yjit / [spinel] / naruby plain/aot/pg / gcc -O0..O3
                     # comparison on bench/fib.na.rb (set SPINEL_DIR=… for spinel row)
make clean
```

If your environment can't write to ccache (`~/.cache/ccache`, common in
sandbox profiles), set `CCACHE_DISABLE=1` — naruby's runtime invokes
`make` to compile SD\_\* objects, and that path goes through the system
`cc`.

## Usage

### Run a script

```sh
./naruby file.na.rb
```

The first run executes the program in plain (interpreted) mode and
then bakes specialized code (`SD_<hash>.c`) into `code_store/c/`,
builds `code_store/all.so` via `make`, and dlopen's it on exit.  Every
subsequent run starts by `dlopen`ing the existing `all.so` and binds
each AST node to its `SD_<hash>` symbol via `astro_cs_load`, so hot
code runs through the specialized dispatchers from the first node
visit.

```ruby
# bench/fib.na.rb

def fib(n)
  if n < 2
    1
  else
    fib(n - 2) + fib(n - 1)
  end
end

fib 40
```

```sh
./naruby bench/fib.na.rb
# Result: 165580141, node_cnt:27
# (first run: also writes code_store/c/SD_*.c and links all.so)

./naruby -b bench/fib.na.rb
# Result: 165580141, node_cnt:27
# (subsequent runs: dlopens code_store/all.so, no rebake)
```

### Modes

| Flag | Mode | What it does |
|---|---|---|
| (none) | Plain + AOT bake | Run interpreted, then bake `code_store/all.so` |
| `-i` | Plain | No AOT load, no bake |
| `-b` | Benchmark mode | Skip bake (load is still done).  Use after a warm-up run |
| `-c` | Compile only | Bake `code_store/all.so` without running the program |
| `-s` | Static-lang | Use `node_call_static` (callee resolved at parse time) |
| `-p` | Profile-guided | Use `node_call2` (cached body slot patched on first call) |
| `-a` | Record all | Register every ALLOC into `code_repo` (more specialize candidates) |
| `-j` | JIT | Submit hot nodes to the L1 worker via Unix socket |
| `-q` | Quiet | Suppress non-essential output |
| `-h` | Help | Print options |

Flags can be combined: `./naruby -c -p file.rb` bakes the PG-flavored
AST without executing.  See [docs/runtime.md](docs/runtime.md) §5 for
how the modes interact.

### Cleaning the code store

```sh
rm -rf code_store
```

Whenever `node.def`, `naruby_gen.rb`, or any source file the SDs depend
on changes, the cached SDs become stale.  `make clean` deletes
`code_store/` along with the generated host-side files.

## Architecture in 10 lines

```
foo.na.rb
  └─ Prism (pm_node_t)
       └─ naruby_parse.c transduce  ──► NODE * (ASTroGen format)
            └─ OPTIMIZE(ast)
                 ├─ -j: astro_jit_submit_query (UDS to L1 worker)
                 └─ default: astro_cs_load(n, NULL) → dlsym SD_<hash>
            └─ EVAL(c, ast)  =  (*ast->head.dispatcher)(c, ast)
       └─ build_code_store: astro_cs_compile / build / reload
```

The hash that keys SDs in `code_store/all.so` is computed structurally
from the AST plus user-defined fold functions for naruby-specific
operands (`hash_builtin_func` for cfunc identity).  The runtime piece
of this is shared with all other ASTro samples — see
[`../../runtime/astro_node.c`](../../runtime/astro_node.c) and
[`../../runtime/astro_code_store.c`](../../runtime/astro_code_store.c).
