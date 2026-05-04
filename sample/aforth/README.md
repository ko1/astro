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

Sustained-scale (~1 s on interp) bench results, gcc-13 -O2, x86_64 Linux,
best-of-3, with gforth 0.7.3 (mature direct-threaded Forth) for context:

| bench         | interp (s) | aot (s) | gforth (s) | aot vs gforth |
|---------------|-----------:|--------:|-----------:|--------------:|
| ack           | 1.536      | 0.509   | 0.493      | 0.97×         |
| array_sum     | 1.219      | 0.154   | 0.472      | **3.06×**     |
| collatz       | 1.101      | 0.071   | 0.497      | **7.00×**     |
| factorial     | 2.363      | 0.644   | 2.210      | **3.43×**     |
| fib           | 0.889      | 0.322   | 0.851      | **2.64×**     |
| gcd           | 1.710      | 0.267   | 0.763      | **2.86×**     |
| nested_loop   | 1.092      | 0.384   | 0.326      | 0.85×         |
| sieve         | 0.745      | 0.079   | 0.375      | **4.75×**     |
| tak           | 0.527      | 0.053   | 0.132      | **2.49×**     |

`aforth+aot` wins 7/9 against gforth (up to 7× on collatz). The two ties
(ack, nested_loop) sit at the indirect-dispatch floor — gforth's DTC NEXT
and `node_call`'s table-load take comparable cycles. Wins concentrate on
inner loops where the body folds into a single SD that gcc can unroll /
hoist; see `docs/perf.md` for why and what could move further.

## How AOT works (one paragraph)

`--aot-compile` calls `astro_cs_compile(entry, NULL)` for the toplevel
node and every `: word ;` body, then `astro_cs_build` produces
`code_store/all.so`. On any subsequent run, `astro_cs_init` dlopens
`all.so`, and `OPTIMIZE` patches each parsed NODE's dispatcher to its
specialized `SD_<hash>` symbol. From then on `EVAL(c, n)` jumps straight
into the SD without going through `DISPATCH_xxx`.
