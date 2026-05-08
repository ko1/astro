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
don't use the `unevaluated_*` keywords.  Currently unconditional (not
conditioned on `c->eval_keys != Qnil`).  Cost ≈ 4 register saves + 4
stores + 4 restores per sub-schema descent.

Measured on user_object (with no `unevaluated_*` in the schema): no
visible regression in the headline bench above (~1.1M ips both directions),
because the writes are to the same VALUE the read just produced and the
compiler / hardware coalesces them well.  Conditionalising the
save/restore is on the todo list as "branch on `c->eval_keys != Qnil`"
in `docs/todo.md`.

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
