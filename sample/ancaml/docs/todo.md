# AnCaml — limitations & TODO

Most of the original TODOs are done (tail-call elimination, 64-bit literals,
leaf-frame allocation, a real `perf record` in [perf.md](perf.md)).  What
remains is either a deliberate design choice or a low-priority polish item.

## Deliberate design choices (not defects)

- **Unknown free names are an error, not externals.** The MinCaml *compiler*
  treats any unbound identifier as an external of fresh type, because it links
  against `libmincaml` and the program may declare its own C externals.  An
  *interpreter* cannot call an arbitrary unknown C function, so `ancaml` restricts
  free names to the known built-in set ([spec.md](spec.md) §External functions)
  and reports "unbound variable" otherwise.  This is the correct behaviour for
  a tree walker, not a missing feature.
- **`print_float` formatting** follows OCaml's `string_of_float` (`%.12g` with a
  trailing `.`), which is what the differential oracle uses; other MinCaml
  runtimes may format floats differently.  Tests prefer `truncate` + `print_int`
  for portable float output regardless.

## Open polish items (low priority)

- **Unboxed floats.** Float-heavy code spends ~11% in `ac_make_float` + GC
  (see [perf.md](perf.md)).  Removing it requires a value-representation change
  (NaN-boxing or an int/float split) — a larger redesign of `context.h`, out of
  scope for the current uniform tagged model.
- **Application inline cache.** A per-call-site cache of the resolved
  closure/body would skip the `node_app*` type/arity check on repeat calls.
  Small win; needs a `@ref` operand, i.e. a tiny `ancaml_gen.rb` subclass (cf.
  `astocac_gen.rb`).
- **`Array.blit` / `Array.length`-style externals.** MinCaml programs rarely
  need them; add if a ported program does.

## Coverage TODO

- A float-heavy program closer to MinCaml's raytracer subset.
- More differential fixtures as ported MinCaml test programs accumulate.
