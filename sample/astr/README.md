# astr — R subset on the ASTro framework

A tree-walking interpreter for an R subset, built with ASTroGen.
The runtime layout follows `sample/naruby` (3-arg dispatcher with an
explicit frame pointer + `RESULT`-typed return for non-local exits via
`return`).  Heap memory uses Boehm-Demers-Weiser conservative GC
(`libgc`).

## Supported language

```r
# arithmetic and assignment (`<-` and `=` both bind)
x <- 1 + 2 * 3
y = x ^ 2 + 17 %% 5

# control flow
if (x > 5) {
    print(x)
} else {
    print(-x)
}
while (cond) { ... }
for (i in 1:n) { ... }
for (v in c(1, 2, 3)) { ... }

# user functions
fib <- function(n) {
    if (n < 2) n
    else       fib(n - 1) + fib(n - 2)
}
print(fib(20))

# strings, vectors, lists
greeting <- paste("hello", "world")
v <- c(1, 2, 3, 4, 5)
print(sum(v))
print(v[3])
v[2] <- 100
```

### VALUE encoding

Tagged 64-bit (`int64_t`):

| low bit | meaning             |
|---------|---------------------|
| `1`     | fixnum (signed 63-bit) |
| `0`     | pointer to `struct astr_obj` (8-byte aligned) |

`TRUE` / `FALSE` collapse to fixnums `1` / `0` so `if (TRUE)` stays in
registers.  Heap object types: `FLOAT`, `STRING`, `NUM_VEC`, `INT_VEC`,
`STR_VEC`, `LIST`, plus `NA` and `NULL` singletons.

### Built-ins

`print`, `cat`, `length`, `c`, `paste`, `paste0`, `nchar`, `substr`,
`floor`, `ceiling`, `sqrt`, `abs`, `log`, `exp`, `sin`, `cos`, `tan`,
`round`, `as.integer`, `as.numeric`, `is.numeric`, `is.character`,
`sum`.

### What's not done yet

- closures with lexical scope beyond top-level
- `apply` / `sapply` family
- the integer/double type distinction (`L` suffix is parsed but ignored
  — integer literals without `.` go to fixnum)
- multi-element subscript (`v[1:3]`, `v[v > 0]`)
- assignment via `[[ ]]`, `$` (only `[ ]` works)
- `tryCatch` / signal handling
- regex / `grepl`
- `data.frame`, S3 / S4 / R6
- profile-guided AOT

## Build

```
make                # builds `astr`
./astr test.r
make test           # runs test/*.r
make bench          # benchmarks (compares against GNU R if Rscript is on PATH)
```

`-rdynamic` is essential — the AOT-built `code_store/all.so` references
host symbols (`astr_resolve_body`, builtin C functions) that the host
binary must export.

## Modes

| flag                 | what it does                                  |
|----------------------|-----------------------------------------------|
| (default)            | run, consulting `code_store/all.so` if present |
| `-i` / `--plain`     | interpreter only                              |
| `-c` / `--aot`       | AOT-bake SDs before run, then run             |
| `--aot-compile`      | bake only, don't run                          |
| `--ccs`              | wipe `code_store/` before run                 |
| `--dump-ast`         | print the parsed AST                          |
| `-q`                 | quiet (no `Result:` line)                     |

`-c` invokes `astro_cs_compile` for the program AST and every
registered function body, links them via the framework's runtime
build (`make -j` under `code_store/`), and dlopens `all.so`.
`CCACHE_DISABLE=1` is set automatically by the bake so ccache
sandboxing failures don't surface.

## Benchmarks (Linux x86_64, gcc -O3, single-threaded)

| bench       | astr -i | astr -c (cached) | speedup |
|-------------|---------|------------------|---------|
| fib(36)     | 0.81 s  | 0.20 s           | **4.1x** |
| loop(50M)   | 0.91 s  | 0.90 s           | 1.0x    |
| ack(3,9)    | 0.49 s  | 0.13 s           | **3.8x** |

`fib` and `ack` benefit from in-SD recursion inlining (the SD specialiser
folds the recursive `cc->body` call into a tight basic block).  The
tight integer loop benchmark is already nearly optimal in the
interpreter — its overhead is dominated by the `while` / `lset` /
`add` cycle which doesn't gain much from specialisation.

If `Rscript` is on your `PATH`, `make bench` will add an `R-base` row
for direct comparison against GNU R.
