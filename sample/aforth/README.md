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
best-of-3:

| bench         | interp (s) | aot (s) | speedup |
|---------------|-----------:|--------:|--------:|
| ack           | 1.746      | 0.521   | 3.4×    |
| array_sum     | 1.200      | 0.152   | 7.9×    |
| collatz       | 1.324      | 0.074   | 17.9×   |
| factorial     | 2.506      | 0.656   | 3.8×    |
| fib           | 1.110      | 0.322   | 3.4×    |
| gcd           | 1.797      | 0.277   | 6.5×    |
| nested_loop   | 1.143      | 0.390   | 2.9×    |
| sieve         | 0.902      | 0.081   | 11.1×   |
| tak           | 0.572      | 0.055   | 10.4×   |

The wins concentrate on inner loops (`collatz`, `sieve`, `tak`,
`array_sum`) where the body fits in a single SD that gcc's loop-pass can
fold into a tight basic block. Recursion-bound benches (`fib`, `ack`,
`factorial`) sit at the floor of the runtime indirect-call cost — see
`docs/perf.md` for why and what could move further.

## How AOT works (one paragraph)

`--aot-compile` calls `astro_cs_compile(entry, NULL)` for the toplevel
node and every `: word ;` body, then `astro_cs_build` produces
`code_store/all.so`. On any subsequent run, `astro_cs_init` dlopens
`all.so`, and `OPTIMIZE` patches each parsed NODE's dispatcher to its
specialized `SD_<hash>` symbol. From then on `EVAL(c, n)` jumps straight
into the SD without going through `DISPATCH_xxx`.
