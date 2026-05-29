# AnLox performance

`make bench` (`ruby benchmark/run_bench.rb`) compares the pure interpreter with
the AOT (code-store) build; outputs are verified equal first.  There is no
system Lox to compare against (set `ANLOX_REF` for a reference column).

## Numbers

Machine: this dev box, `gcc -O2`, libgc.

| program | interp (s) | AOT (s) | AOT vs interp |
|---|---|---|---|
| `fib 30` (recursive) | 0.33 | 0.34 | ~1.0× |
| `loop 5e6` (local-var while) | 0.50 | 0.46 | ~1.1× |

## Reading the numbers

AOT gives little here, and the reason is structural (verifiable from `value.c`
/ `node.def`, not a profile):

- **Every number is a boxed `double`.** `n - 1`, `s + i`, `i + 1` each allocate
  a fresh `LOX_NUM` on the GC heap.  Arithmetic-heavy code is dominated by that
  allocation + collection, which AOT does **not** remove — `astro_cs_compile`
  folds the per-node dispatch chain, not the boxing.  This is the same effect
  seen in `ancaml`'s float benchmark, but more pronounced because in Lox *all*
  numbers are boxed.
- **Globals are looked up by name.** `fib` is a global, so each recursive call
  does a string-hash lookup in the globals table (`lox_global_get`).  That cost
  is independent of dispatch and survives AOT.
- **Per-call frame allocation.** `lox_call` `GC_MALLOC`s a frame per call (no
  leaf-frame `alloca` like `ancaml`, since closures may escape).

So AOT folds the dispatch chain into one SD, but the hot costs (boxing, global
hashing, frame allocation) lie outside it — hence the flat result.

## Where the headroom is (see [todo.md](todo.md))

1. **Unboxed numbers** — a value-representation change (NaN-boxing, as `clox`
   uses, or a tagged immediate small-number path).  This is the single biggest
   lever and would change `context.h` substantially.
2. **Global resolution** — cache the resolved global cell per `node_global`
   site (an inline cache), turning the per-access hash lookup into a pointer
   load.
3. **Leaf-frame `alloca`** — stack-allocate call frames for non-escaping
   functions, as `ancaml` does.

A `perf record` attribution has not been run yet; the points above are
structural facts about the implementation, not a profile.
