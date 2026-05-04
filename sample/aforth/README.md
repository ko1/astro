# aforth — Forth subset on ASTro

`aforth` is a Forth-subset interpreter built on the ASTro framework. Unlike
the traditional threaded-code Forth implementations, every Forth word here
is a NODE in an AST; ASTroGen generates the dispatch / specialize / hash
machinery from `node.def`, and the runtime [code store][code-store] caches
specialized SDs (per-word compiled C functions) on disk so that repeat
runs reuse them.

[code-store]: ../../runtime/astro_code_store.h

## Quick start

```sh
make                                 # build aforth
./aforth -q test/test_def.fs         # run a script (interpreter only)
./aforth -q --aot-compile bench.fs   # compile every entry, then run
ruby benchmark/run.rb                # full benchmark table
```

## Files

| File | Purpose |
|------|---------|
| `node.def` | Forth word semantics (one `NODE_DEF` per primitive + control structure) |
| `node.h` / `node.c` | Per-sample type declarations + `EVAL` / `OPTIMIZE` / `INIT` |
| `context.h` | `CTX` (data stack, return stack, DO-loop frame stack, vars area) |
| `main.c` | Tokenizer + recursive-descent parser + AOT pipeline |
| `Makefile` | Build, regen, test, bench, aot-build targets |
| `benchmark/` | Sustained-scale benchmarks (~1 s on interp) |
| `test/` | Smoke tests covering arithmetic / stack / control-flow / definitions |
| `docs/` | done.md / todo.md / perf.md / runtime.md |

The directory layout follows `sample/koruby/`.

## Forth subset

See [docs/done.md](./docs/done.md) for the full word list. In brief:

- Core stack ops: `DUP DROP SWAP OVER ROT NIP TUCK 2DUP 2DROP ?DUP DEPTH`
- Arithmetic: `+ - * / MOD NEGATE ABS 1+ 1- 2* 2/`
- Comparison: `= <> < > <= >= 0= 0< 0>`  (Forth-true is `-1`)
- Bitwise: `AND OR XOR INVERT LSHIFT RSHIFT`
- Return-stack: `>R R> R@`
- Control flow: `IF ELSE THEN`, `BEGIN ... UNTIL`, `BEGIN ... AGAIN`,
  `BEGIN ... WHILE ... REPEAT`, `DO ... LOOP`, `DO ... +LOOP`, `I J LEAVE`
- Definitions: `: NAME ... ;` with `RECURSE`
- Storage: `VARIABLE`, `<n> CONSTANT NAME`, `CREATE NAME`, `<n> ALLOT`,
  `@ ! +! CELLS CELL+`
- I/O: `. EMIT CR SPACE BL ." string"`
- Comments: `\ ...`, `( ... )`

Not (yet) supported: `EXIT`, `DOES>`, immediate words / compile-time
extension, locals, floats, threads, file I/O.

## Performance

Sustained-scale (~1 s on interp) bench results, gcc-13 (SDs at -O3 -flto
-march=native), x86_64 Linux, best-of-5, with gforth 0.7.3 (mature
direct-threaded Forth) for context:

| bench         | interp (s) | aot (s) | gforth (s) | aot vs gforth |
|---------------|-----------:|--------:|-----------:|--------------:|
| ack           | 1.677      | 0.513   | 0.494      | 0.96×         |
| array_sum     | 1.367      | 0.088   | 0.541      | **6.15×**     |
| collatz       | 1.212      | 0.069   | 0.494      | **7.16×**     |
| factorial     | 2.530      | 0.286   | 2.133      | **7.46×**     |
| fib           | 1.037      | 0.314   | 0.761      | **2.42×**     |
| gcd           | 1.863      | 0.052   | 0.770      | **14.81×**    |
| nested_loop   | 1.142      | 0.086   | 0.350      | **4.07×**     |
| sieve         | 0.920      | 0.077   | 0.395      | **5.13×**     |
| tak           | 0.561      | 0.055   | 0.131      | **2.38×**     |

`aforth+aot` wins 8/9 against gforth (up to 14.8× on gcd). The single
near-tie (ack, 0.96×) is deep `RECURSE` whose `node_call` indirect
dispatch floor matches gforth's DTC NEXT. Wins concentrate on inner
loops where the body folds into a single SD that gcc can unroll /
hoist; see `docs/perf.md` for two perf rounds: (1) `restrict`-tuning
that moved 4 benches by 1.6× to 4.7×, and (2) `-flto` on `all.so` for a
flat 3-10 % across the table.

## How AOT works (one paragraph)

`--aot-compile` calls `astro_cs_compile(entry, NULL)` for the toplevel
node and every `: word ;` body, then `astro_cs_build` produces
`code_store/all.so`. On any subsequent run, `astro_cs_init` dlopens
`all.so`, and `OPTIMIZE` patches each parsed NODE's dispatcher to its
specialized `SD_<hash>` symbol. From then on `EVAL(c, n)` jumps straight
into the SD without going through `DISPATCH_xxx`.
