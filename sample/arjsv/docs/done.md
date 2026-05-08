# arjsv: implemented features

draft-04 / draft-06 / draft-07 / 2019-09 / 2020-12 を `$schema` で
auto-detect。 json_schemer-compatible API。

詳細仕様は [`spec.md`](./spec.md)、 内部実装は [`runtime.md`](./runtime.md)、
ベンチは [`perf.md`](./perf.md)、 残作業は [`todo.md`](./todo.md)。

## Public API

```ruby
require 'arjsv'

# 基本
s = Arjsv.schema({'type' => 'integer', 'minimum' => 0})
s.valid?(42)         # => true (fast path, arjsv の specialised dispatcher)
s.validate(42).to_a  # => [] (fast path)
s.validate(-1).to_a  # => [{ ... json_schemer 互換 error hash ... }]
s.compile!           # AOT-specialise (default は interp、 hot loop で呼ぶなら推奨)

# json_schemer-compatible options
Arjsv.schema(schema, formats: { 'phone' => ->(s) { ... } })
Arjsv.schema(schema, insert_property_defaults: true)

# meta-schema 検証
Arjsv.valid_schema?(schema_obj)
s.valid_schema?

# Symbol key data も透過に動く
s.valid?({name: 'Alice'})            # ← Symbol key OK
s.valid?({'name' => 'Alice'})        # ← String key も OK
```

`valid?` は arjsv 内で完結。 `validate` は失敗時のみ json_schemer に
委譲して rich error 配列を返す (happy path は arjsv 速度を維持)。

## Supported keywords (draft-07 + 2020-12)

2020-12 keywords are normalised to draft-07 forms during lowering —
`prefixItems` → tuple `items`, `dependentRequired`/`dependentSchemas` →
`dependencies`, `$dynamicRef` → `$ref`, `$dynamicAnchor`/`$anchor` →
local id-map.  `unevaluatedItems` / `unevaluatedProperties` are
implemented via a runtime `eval_keys` Hash + `eval_items` count, with
`eval_scope` propagating annotations up from in-place applicators
(allOf members, $ref bodies) on success.  `$id` / `$ref` resolution
follows RFC 3986 base-URI rules.

| Group  | Keyword              | Notes |
|--------|----------------------|-------|
| meta   | `type`               | string or array of strings (`null bool int num str arr obj`) |
| meta   | `const`              | any JSON value |
| meta   | `enum`               | any JSON values |
| object | `required`           | array of strings |
| object | `properties`         | hash → sub-schema |
| object | `additionalProperties` | `false` (no extra keys) or sub-schema |
| object | `patternProperties`  | regex → sub-schema (each matching key checked against schema) |
| object | `propertyNames`      | sub-schema applied to each key |
| object | `minProperties`      | integer |
| object | `maxProperties`      | integer |
| array  | `items`              | single sub-schema (uniform) or array of sub-schemas (tuple) |
| array  | `additionalItems`    | `false` or sub-schema (paired with tuple `items`) |
| array  | `minItems`           | integer |
| array  | `maxItems`           | integer |
| array  | `uniqueItems`        | bool — deep `rb_equal` per pair |
| number | `minimum`            | inclusive |
| number | `maximum`            | inclusive |
| number | `exclusiveMinimum`   | numeric (draft-07) and bool (draft-04 fallback) |
| number | `exclusiveMaximum`   | numeric (draft-07) and bool (draft-04 fallback) |
| number | `multipleOf`         | divisor (any Numeric) |
| string | `minLength`          | character count, encoding-aware |
| string | `maxLength`          | character count, encoding-aware |
| string | `pattern`            | Ruby Regexp (precompiled at schema build) |
| string | `format`             | subset: date / date-time / time / email / uri / ipv4 / ipv6 / uuid; unknown formats are annotation-only |
| combinator | `allOf`          | chained AND |
| combinator | `anyOf`          | chain w/ early-success |
| combinator | `oneOf`          | counter-walk; exactly 1 match |
| combinator | `not`            | invert |
| combinator | `if`/`then`/`else` | branch on if-schema |
| ref    | `$ref`               | `#`, `#/$defs/<name>`, `#/definitions/<name>`, general `#/<json-pointer>`, `<$id>` lookup; recursive refs supported |
| ref    | `$defs` / `definitions` / `$anchor` | each entry compiles as its own SD; cyclic refs resolved via lazy slot |
| dep    | `dependencies` (draft-07) / `dependentRequired` + `dependentSchemas` (2020-12) | unified per-key chain |
| array  | `contains` (+ `minContains` / `maxContains`) | counts matches against the schema |
| 2020-12 | `prefixItems`        | normalised to tuple `items` |
| content | `contentEncoding` (`base64`) / `contentMediaType` (`application/json`) | asserted in draft-07; annotation-only in 2020-12 |

Boolean schemas: `true` (always valid), `false` (always invalid).
Empty `{}` schema accepted.

## JSON Schema Test Suite

| draft     | pass | total | % |
|-----------|---:|---:|---:|
| draft-04  |  906 |  917 | **98.80%** |
| draft-06  | 1196 | 1209 | **98.92%** |
| draft-07  | 1513 | 1584 | **95.52%** |
| 2020-12   | 1957 | 2069 | **94.59%** |

Run with `ruby test/run_official_suite.rb` (default = draft-07);
`DRAFT=draft4|draft6|draft2020-12 …` switches dataset.

### draft-07 failure breakdown (71 fails)

| group | fails | reason |
|---|---:|---|
| `optional/format/idn-hostname` | 34 | IDNA-2008 contextual rules; needs libidn / ICU |
| `optional/format/hostname` | 23 | A-label punycode contextual rules; same |
| `refRemote.json` | 11 | external `$ref` (HTTP fetching); out of scope |
| `ref.json` | 1 | meta-schema HTTP fetch |
| `definitions.json` / `optional/cross-draft.json` | 1 each | external HTTP $ref |

### 2020-12 failure breakdown (112 fails)

| group | fails | reason |
|---|---:|---|
| `optional/format/idn-hostname` | 34 | IDNA / Unicode tables |
| `optional/format/hostname` | 23 | A-label punycode |
| `format.json`             | 18 | tradeoff: arjsv asserts format unconditionally; the 18 `valid` cases here include strings like `127.0.0.1.0` for `format: ipv4` that real validators rightly reject |
| `dynamicRef.json`         | 16 | full dynamic-anchor scope resolution not implemented |
| `refRemote.json`          | 15 | external HTTP fetch |
| `unevaluatedItems.json`   | 2 | `contains`-tracked sparse indices (current `eval_items` is prefix-count) |
| `vocabulary.json` / `defs.json` / `ref.json` / `ecmascript-regex` | 1 each | meta-schema HTTP, custom vocab, etc. |

## Out of scope (by design)

- External `$ref` (HTTP fetching) — `refRemote.json`, plus the
  meta-schema cases in `definitions.json` / `cross-draft.json` /
  `vocabulary.json` that fetch the spec metaschema URL.
- IDNA-2008 / punycode validation for `format: hostname` / `idn-hostname`
  / `idn-email` Unicode-form validation — needs libidn or ICU tables.
- `$dynamicRef` full dynamic-scope resolution — implemented as a static
  `$ref` to the matching `$dynamicAnchor`.  Static cases work; the cases
  that distinguish dynamic-vs-static scope do not.

## Known small gaps

- `unevaluatedItems` with `contains` (2 tests in 2020-12): the current
  `c->eval_items` is a prefix-length integer.  Fixing requires switching
  to a sparse-set representation; deferred.
- Strict ECMA-262 control-escape rejection in `pattern` (1 test): we
  accept `\a` because Onigmo does; ECMA forbids it.

## Internals

- AST built via Ruby walker (`lib/arjsv.rb`) calling `_alloc_*` C entries
- Each NODE wrapped as `T_DATA` (`Arjsv::Node`) for GC reachability via children
- Schema holds the root NODE wrapper + a Ruby Array of constants for `enum` /
  `const` / property fstrings / `pattern` / `format` regexes / `$defs` targets,
  plus an Array of all entry NODEs (root + secondary entries needing
  independent SD registration)
- `compile!` iterates all entries calling `astro_cs_compile` + one
  `astro_cs_build` + `astro_cs_reload` + per-entry `astro_cs_load`, with
  Ruby header cflags injected (`Arjsv::RUBY_HEADER_CFLAGS`)
- ~45 node kinds: validate_root, pass, fail, seq, type_check, required(+_unsafe),
  property(+_unsafe, +_with_default), items_uniform(+_unsafe), items_tuple,
  additional_items, no_additional_items, min_items, max_items, unique_items,
  min_properties, max_properties, pattern_property, additional_properties_schema,
  no_additional_properties, property_names, minimum, maximum, multiple_of,
  min_length, max_length, pattern, format, content_check, const, enum, not,
  if_then_else, any_of, one_of, one_of_step, ref, dependency, contains,
  eval_scope, unevaluated_properties_schema, no_unevaluated_properties,
  unevaluated_items_schema, no_unevaluated_items
- **Type-guard fast path**: when a schema's `type` is the single string
  "object" (resp. "array"), `node_property_unsafe` /
  `node_required_unsafe` (resp. `node_items_uniform_unsafe`) skip the
  per-call `RB_TYPE_P` since the parallel `node_type_check` already
  guarded the data type via the `seq` short-circuit
- **Format fast path**: formats whose validation is pure regex (email,
  uuid, ipv4 — see `lib/arjsv/format.rb`'s `REGEX_SHORTCUTS`) dispatch
  through `node_pattern` (one `rb_reg_match`) instead of `node_format`
  (`rb_funcall` → Ruby Proc).  Saves ~700 ns / format check on the hot
  path.  Stdlib-parsing formats (date, time, uri, ipv6, hostname, …)
  still go through Procs
- **Symbol-key data**: `node_property` / `node_required` / `node_dependency`
  try the String form first (the JSON spec form, also `JSON.parse`
  default — full speed in this case) and fall back to the Symbol form
  only on miss.  String-key callers see no overhead; Symbol-key callers
  pay one extra `rb_hash_lookup2` per property check
- **Symbol-key schemas**: `Arjsv.schema(type: :integer, ...)` is
  normalised to String-keyed at build time (one shot); `enum` / `const`
  / `default` / `examples` *values* are left untouched so the user's
  data shapes still match
- **Annotation tracking** (for `unevaluatedProperties` / `unevaluatedItems`):
  `c->eval_keys` (Hash) and `c->eval_items` (count) are populated by
  successful property / pattern_property / additional_properties /
  items_uniform / items_tuple / additional_items / contains nodes when
  the surrounding `eval_scope` has activated tracking.  Combinators
  (`anyOf` / `oneOf` / `not` / `if-then-else`) implement the spec's
  evaluation propagation rules (only matched branches contribute;
  `not` blocks contribution; failing branches roll back via `dup`)
- Properties / required / enum / pattern_property / etc. are right-recursive
  chains terminated by `pass` / `fail`
- Property names are interned as frozen Ruby Strings in the Schema's `consts`
  Array; runtime lookup is `rb_hash_lookup2(data, c->consts[idx])` — zero
  allocation per validate call.  See `docs/perf.md` §"Fix 1".
- `$ref` uses lazy slot indirection: each `$defs` name reserves a `consts`
  slot at preregistration time, then the body lowering writes the
  validate_root wrapper into the slot.  Recursive `$ref`s pick up *the slot*
  during lowering and read the wrapper from it at runtime.  Three ref
  resolution paths are supported in order:
    1. `#`, `#/`              → root validate_root slot
    2. `#/$defs/<name>` and `#/definitions/<name>` (single segment)
                             → preregistered slot (forward refs OK)
    3. General `#/path/seg/...`  → walk the original schema hash, lower
                                   the pointed-at sub-schema lazily
    4. `$id`-based ref       → match against an `$id`-map collected by
                               a one-shot walk before lowering
- `additionalProperties` / `patternProperties` / `propertyNames` use
  `rb_hash_foreach` with a closure struct carrying the target NODE's
  dispatcher (read at runtime).  Their schema bodies are listed as secondary
  entries for compile.
