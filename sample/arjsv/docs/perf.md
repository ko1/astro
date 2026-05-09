# arjsv performance notes

draft-04 / 06 / 07 / 2019-09 / 2020-12 (auto-detected via `$schema`).
Full keyword matrix in [`done.md`](./done.md), implementation in
[`runtime.md`](./runtime.md).  Bench schemas exercise `format: email`,
`additionalProperties: false`, `$ref`, `enum`, etc. — representative of
real OpenAPI workloads.

## 3-way benchmark (ruby 4.0.2 / x86_64 / `benchmark/run.rb`)

Compared against:
- **`json_schemer` 2.5.0** — current de facto pure-Ruby validator
- **`rj_schema` 1.0.5** — Rust/RapidJSON-backed validator (FFI from Ruby)

Two scenarios per schema:
- **(A) parsed Ruby value in** — input is an already-parsed Ruby Hash /
  Array / scalar.  Typical Rails / rack flow where middleware has parsed
  the request body.  arjsv + json_schemer take this directly; rj_schema
  has to `JSON.generate` first because its API only accepts JSON strings.
- **(B) JSON string in** — input is a raw JSON string.  Gateway / streaming
  case.  arjsv + json_schemer call `JSON.parse` first; rj_schema gets
  fed the string natively (preloaded schema, RapidJSON-internal parsing).

### (A) parsed Ruby value in

| schema | json_schemer | arjsv interp | arjsv AOT | rj_schema (Hash→JSON) | arjsv vs rj_schema |
|---|---:|---:|---:|---:|---:|
| simple int (valid)        |  182k |  14.83M |  14.87M |  606k | **25×** |
| user object (valid)       | 11.5k |   1.12M |   1.11M |  125k | **8.9×** |
| user object (invalid)     | 62.6k |  11.13M |  11.21M |  130k | **86×** |
| api response x5  (valid)  | 1.60k |    136k |    133k | 18.2k | **7.5×** |
| api response x50 (valid)  |   163 |   13.6k |   13.3k | 1.97k | **6.9×** |

### (B) JSON string in

| schema | json_schemer (parse+v) | arjsv interp (parse+v) | arjsv AOT (parse+v) | rj_schema (preloaded) | arjsv vs rj_schema |
|---|---:|---:|---:|---:|---:|
| simple int (valid)        |  179k |   6.21M | 6.24M  |  712k | **8.8×** |
| user object (valid)       | 11.2k |    545k |  555k  |  137k | **4.0×** |
| user object (invalid)     | 59.8k |   1.61M | 1.61M  |  143k | **11×** |
| api response x5  (valid)  | 1.54k |  77.4k  | 77.3k  | 18.7k | **4.1×** |
| api response x50 (valid)  |   157 |  8.69k  | 8.73k  | 2.07k | **4.2×** |

`api response x5` and `x50` exercise `$ref` (recursive User+Address),
`additionalProperties:false`, `pattern`, `format`, `enum`, `uniqueItems`,
`minItems` / `maxItems` — i.e. the bench is on a realistic OpenAPI-flavoured
schema, not just primitives.

### Headline takeaways

- **arjsv ≥ 4× faster than `rj_schema` (Rust+RapidJSON) in scenario B**
  (gateway flow, rj_schema's native form), and 7×–86× in scenario A.
- The win over Rust comes from two places: rj_schema pays an FFI call
  boundary plus per-call data-JSON parsing on every validate; arjsv takes
  parsed Ruby objects directly and stays in-process.  Even when arjsv
  pays `JSON.parse` (scenario B), CRuby's parser + arjsv's specialised
  validator beats RapidJSON parse + RapidJSON validate.
- vs `json_schemer`: 25×–180× across the matrix.

## Fix 1: per-`node_property` / `node_required` fstring cache

(Carried over from the Tier 1 phase — the change that took user-object
validation from 640 ns → 139 ns, before further keyword work.  See git log
for the commit.)

Initial implementation called `rb_str_new_cstr(key)` per property check,
allocating a fresh String each time.  The fix interns each property name
once at schema build time and indexes it from the node:

- `const char *key` operand — content-hashed (FNV) for SD-cache
  correctness across schemas with different keys.
- `uint32_t key_idx` operand — index into `c->consts[]`, where the frozen
  Ruby String lives.  Runtime lookup becomes
  `rb_hash_lookup2(c->data, c->consts[key_idx])` — zero allocation,
  hash precomputed (Ruby caches `String#hash` on frozen strings).

`perf record` post-fix shows `gc_sweep_step` and `str_enc_new` are gone;
`SD_<root>` (the AOT specialised dispatcher) now takes ~16 % of CPU,
i.e. it's doing real work, not just dispatch.

## Spec-compliance overhead

The 2019-09+ `unevaluatedProperties` / `unevaluatedItems` work added a
per-call save/restore of `c->eval_keys` and `c->eval_items` to every
property / items / pattern_property / etc. node — even for schemas that
don't use the `unevaluated_*` keywords.  Currently unconditional.

We tried gating the save/restore on `c->eval_keys != Qnil` (so schemas
without `unevaluated_*` skip the work entirely; in those schemas the
saved value is always Qnil/-1 anyway, so the writes are dead).  Result:
**no measurable change** on user_object (303k ± 5% vs 308k ± 4%
baseline) and array-of-150-ints (334k vs 334k).

Why: the unconditional 4 register saves + 4 stores + 4 restores hit L1
and the store buffer absorbs them; the writes are to the same VALUE
the read just produced and the OOO core coalesces.  Branch + skip
saves the L1 traffic but doesn't move the wall clock because the rest
of the per-node cost (`rb_hash_lookup2` ~25 ns) is an order of
magnitude bigger.

The optimisation is dropped from the todo list: cleaner code, no perf
gain.  This is the typical shape of arjsv's perf landscape — the
remaining cost is in CRuby C API calls that arjsv can't reach.

## AOT vs interp on small schemas: still ~tied

| schema (scenario A) | interp | AOT | gap |
|---|---:|---:|---:|
| simple int (valid)    |  13.78M |  14.00M | tied |
| user object (valid)   |   1.28M |   1.30M | tied |
| user object (invalid) |  10.93M |  10.65M | tied |
| api response x50      |  16.0k  |  16.1k  | tied |

`objdump` on user-object's SD confirms the entry is a single function with
*no* `call SD_*` / `call DISPATCH_*` — the whole 17-node tree got inlined.
The remaining `call` instructions are CRuby C API only (`rb_hash_lookup2`,
`rb_str_strlen`, etc.).

So why no win over interp?

1. **Interpreter dispatch is well predicted**.  Same N nodes visited
   in the same order every iteration → branch predictor latches onto
   each indirect call's target.  Each `(*head.dispatcher)(c, n)` is
   ~2 ns when hot.
2. **CRuby C API dominates the residual cost**.  `rb_hash_lookup2`,
   `rb_str_hash`, `rb_str_comparable` etc. are ~25 % of the user-object
   profile and are exactly what AOT can't reach into.
3. **Ruby `valid?` method dispatch is fixed** — ~28 % of the user-object
   profile is `vm_exec_core` / `vm_invoke_iseq_block` / `rb_arjsv_schema_valid_p`,
   and is the same in both modes.

The absolute floor: `FakeSchema#valid?` (pure Ruby `def valid?(x); true; end`)
clocks 45 ns/op on this machine.  arjsv interp on `type:integer` is 50 ns:
45 ns floor + 5 ns of validator.  AOT can't go below the floor, and 5 ns
is already gcc-static-inline-everything.

This is the opposite trade-off from naruby / calc, where per-node work
is ~1 ns of arithmetic and dispatch is dominant.  Here per-node work is
a CRuby C API call (~10–80 ns) that the framework can't reach into.

## Why AOT doesn't help on JSON Schema (vs astrogre etc.)

JSON Schema validation is mostly a linear "type check → property loop
→ value check" walk.  Branching is shallow and predictable
(`allOf` / `oneOf` / `if`/`then`/`else` are typically 2-3 way max in
real schemas).  Per-node work is **a CRuby C API call** (~10-80 ns)
that gcc can't inline through.

Compare astrogre (regex engine, same framework):
- per-node work is ~1-2 ns of char compare
- branching is deep (alternation / repetition / lookahead) and
  unpredictable
- AOT fuses the start-position scan loop and the regex chain into one
  SD → 3-15× wins

For arjsv to benefit from AOT-style fusion you'd need a schema with
deep `oneOf` chains (20+ branches), huge flat `enum`s, or other
dispatch-heavy structures.  Real-world OpenAPI schemas don't have those
shapes, so the benchmarks tie.

## Floor

The floor for "Ruby validator that takes a Ruby Hash" is the cost of:
- Ruby `valid?` method dispatch: ~45 ns per call (measured against a
  pure-Ruby `def valid?(_); true; end`)
- one `rb_hash_lookup2` per property: ~25 ns (CRuby internal, untouchable)

So a 4-property `valid?` on a Hash is ~145 ns floor (45 + 4×25 = 145).
arjsv interp on user_object hits ~3 µs (≈ 1 µs is the floor for that
schema's 9-ish lookups + Ruby boundary; rest is format regex + a few
short-circuits).

To push lower you'd need either (a) drop the Ruby Hash API (e.g. take
JSON string + own parser, like rj_schema does — but then you lose the
"already-parsed Hash" speed advantage) or (b) framework-level work to
remove the `c->consts[idx]` indirection (see `docs/todo.md`).

## Methodology

- benchmark-ips, 1 s warmup + 3 s measurement.
- `CCACHE_DISABLE=1` set so `astro_cs_build`'s inner `make` doesn't fail
  on the project's read-only `~/.cache` (sandbox quirk).
- Each schema is compiled once (`schema.compile!`) before the AOT run;
  that cost is excluded from the timing.
- Sanity check at the start of `benchmark/run.rb` confirms json_schemer,
  arjsv (both modes), and rj_schema all agree on the validity of every
  test datum before any timing happens.
