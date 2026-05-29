# AnLox — limitations & TODO

## Known limitations

- **No standalone `--build` executable.** Every node reaches its children
  through the `LOX_BLOCK_STMTS` / `LOX_CALL_ARGS` / `LOX_FUNDEFS` /
  `LOX_CLASS_METHODS` side-tables by index, and the framework's AST embedder
  only reconstructs `NODE*` operands — the side-tables would be NULL in the
  baked exe.  `--build` is rejected with a message.  *The interpreter and
  `--aot-compile` (code_store) modes are fully supported.*  Fixing this needs a
  custom emit that also serializes the side-tables (same gap affects
  `astocaml`'s `appn`/`tuple_n`).
- **No block comments** (`/* */`) — the book's jlox doesn't have them either
  (it's a challenge exercise).
- **Number printing** uses `printf` (`%lld` for integers, `%.10g` otherwise);
  many-digit decimals can differ from jlox's Java formatting.
- A value-returning `return` inside `init` is accepted (the book makes it a
  compile error).
- No tail-call elimination — matching jlox/clox (Lox doesn't require it); deep
  non-tail recursion is bounded by the C stack.

## Performance TODO (see [perf.md](perf.md))

1. **Unboxed numbers** — biggest lever; a value-representation change
   (NaN-boxing or a tagged small-number path).  Currently every arithmetic
   result boxes a `double`.
2. **Global inline cache** — cache the resolved global cell per `node_global`
   site so repeated access is a pointer load, not a hash lookup.
3. **Leaf-frame `alloca`** — stack-allocate non-escaping call frames (cf.
   `ancaml`).
4. A `perf record` breakdown to attribute time before claiming hot spots.

## Coverage TODO

- More fixtures ported from the official Crafting Interpreters test suite
  (string ops, number edge cases, closure/scope corner cases, more resolver
  errors).
- Wire up `ANLOX_REF` against a built `clox` in CI for true differential runs.
