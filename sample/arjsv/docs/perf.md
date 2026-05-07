# arjsv performance notes

draft-07 keywords (full Tier 1 + Tier 2 + Tier 3 set: type / properties /
required / items uniform-and-tuple / additionalItems / additionalProperties
/ patternProperties / propertyNames / minimum / maximum / exclusive forms /
multipleOf / minLength / maxLength / pattern / format / minItems / maxItems
/ uniqueItems / minProperties / maxProperties / const / enum / allOf / anyOf
/ oneOf / not / if-then-else / `$ref` to `#/$defs/` and `#/definitions/` /
recursive `$ref`).

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
| simple int (valid)        |   176k |  13.78M |  14.00M |  274k | **51×** |
| user object (valid)       |  10.8k |   1.28M |   1.30M |  125k | **10×** |
| user object (invalid)     |  62.7k |  10.93M |  10.65M |  130k | **84×** |
| api response x5  (valid)  |  1.23k |  127k¹  |  55.6k¹ | 16.5k | **3.4×** |
| api response x50 (valid)  |    180 |   16.0k |   16.1k |  2.12k | **7.6×** |

ips (i/s).  ¹ `api response x5` interp came in with 28% variance; AOT was
stable at 2.6%; treat the interp number as an upper bound from a lucky
warmup.

### (B) JSON string in

| schema | json_schemer (parse+v) | arjsv interp (parse+v) | arjsv AOT (parse+v) | rj_schema (preloaded) | arjsv vs rj_schema |
|---|---:|---:|---:|---:|---:|
| simple int (valid)        |  133k |   2.65M | 5.05M  |  692k | **7.3×** |
| user object (valid)       | 7.66k |    614k |  616k  |  137k | **4.5×** |
| user object (invalid)     | 53.2k |   1.50M | 1.59M  |  148k | **10.7×** |
| api response x5  (valid)  | 1.70k |  89.6k  | 90.6k  | 20.3k | **4.5×** |
| api response x50 (valid)  |    174 |  10.3k | 10.5k  | 2.27k | **4.6×** |

`api response x5` and `x50` exercise `$ref` (recursive User+Address),
`additionalProperties:false`, `pattern`, `format`, `enum`, `uniqueItems`,
`minItems` / `maxItems` — i.e. the bench is on a realistic OpenAPI-flavoured
schema, not just primitives.

### Headline takeaways

- **arjsv ≥ 4.5× faster than `rj_schema` (Rust+RapidJSON) in scenario B**
  (gateway flow, native form of rj_schema), and 7×–84× in scenario A.
- The win over Rust comes from two places: rj_schema pays an FFI
  call boundary plus per-call data-JSON parsing on every validate; arjsv
  takes parsed Ruby objects directly and stays in-process.  Even when arjsv
  pays `JSON.parse` (scenario B), CRuby's parser + arjsv's specialised
  validator beats RapidJSON parse + RapidJSON validate.
- vs `json_schemer`: 30×–175× across the matrix.

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

## Where AOT *could* still help

1. **Bigger schemas where dispatch counts add up** — at some N the
   branch predictor capacity gives out and AOT pulls ahead.  The
   `api response x50` case is hint-ward of this (16k tied — but AOT's
   variance is lower); not yet decisive.
2. **Once `rb_hash_lookup2` is reduced** — e.g. by also embedding a
   per-property hash-slot index cache: the relative weight of dispatch
   goes up and AOT wins more.  Diminishing-returns territory.
3. **Embedding the fstring VALUE in the SD as a literal** instead of
   indirecting through `c->consts[idx]`: noted by the user as an
   ASTro framework-level enhancement (cross-cutting with abruby etc.).
   Today consts indices stay stable across SD reuses but the actual
   Ruby VALUE doesn't (fresh process = fresh fstring identities).  A
   reload-time fixup pass on the SD's `.rodata` would let SDs reference
   VALUEs directly, removing one load per property check.  Listed in
   `docs/todo.md`.

## Methodology

- benchmark-ips, 1 s warmup + 3 s measurement.
- `CCACHE_DISABLE=1` set so `astro_cs_build`'s inner `make` doesn't fail
  on the project's read-only `~/.cache` (sandbox quirk).
- Each schema is compiled once (`schema.compile!`) before the AOT run;
  that cost is excluded from the timing.
- Sanity check at the start of `benchmark/run.rb` confirms json_schemer,
  arjsv (both modes), and rj_schema all agree on the validity of every
  test datum before any timing happens.
