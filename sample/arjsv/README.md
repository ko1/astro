# arjsv — ASTro Ruby JSON Schema Validator

JSON Schema (draft-07) validator built on the ASTro framework, distributed
as a CRuby C extension.  The schema is lowered to an ASTro AST whose
specialised dispatchers compile down to a tight per-schema validator
function.

Goal: drop-in replacement for `json_schemer.valid?` with substantially
lower per-validation overhead.

## Status

draft-07 + draft 2020-12 (auto-detected from `$schema`).  Keyword
coverage is essentially complete: type / properties / required / items
(uniform + tuple) / additionalItems / additionalProperties /
patternProperties / propertyNames / dependencies (+ 2020-12
`dependentRequired` / `dependentSchemas`) / contains (+ minContains /
maxContains) / size constraints / numeric ranges / multipleOf /
pattern / format (Ruby-stdlib validators for date / time / duration /
email / uri / ipv4 / ipv6 / uuid / json-pointer / etc.) / const / enum /
allOf / anyOf / oneOf / not / if-then-else / contentEncoding /
contentMediaType / `$ref` (root, `#/$defs/<name>`, full JSON pointer,
`$id` lookup; recursive refs supported).

**JSON Schema Test Suite**:
- draft-04:  902 / 917 = **98.36%**
- draft-06: 1185 / 1209 = **98.01%**
- draft-07: 1501 / 1584 = **94.76%**
- 2020-12:  1924 / 2069 = **92.99%**

Remaining failures concentrate in IDNA hostname (no libidn), external
HTTP `$ref`, and a few deep `$dynamicRef` / nested-`$id` cases — see
[`docs/done.md`](docs/done.md) for the breakdown.

json_schemer drop-in API: `valid?`, `validate` (Enumerator of error
hashes), `valid_schema?`, `formats:` 独自 format 拡張、
`insert_property_defaults:` の data mutation も対応。  Symbol-key データ /
Symbol-key スキーマも透過に動く。  詳細は [`docs/spec.md`](docs/spec.md)、
内部設計は [`docs/runtime.md`](docs/runtime.md)、
ベンチは [`docs/perf.md`](docs/perf.md)。

## Build

```sh
ruby extconf.rb && make            # builds arjsv.so + generated ASTro files
make test                           # interpreter mode
ARJSV_MODE=compiled make test       # AOT-specialised mode
make bench                          # vs json_schemer
```

If `make` errors complain about `ccache`, set `CCACHE_DISABLE=1` (sandbox /
read-only `.cache` workaround).

## Usage

```ruby
require 'arjsv'

schema = Arjsv.schema(
  'type' => 'object',
  'required' => ['name'],
  'properties' => {
    'name' => {'type' => 'string', 'minLength' => 1},
    'age'  => {'type' => 'integer', 'minimum' => 0},
  }
)

schema.valid?({'name' => 'Alice', 'age' => 30})   # => true
schema.valid?({'age' => 30})                      # => false (missing name)

schema.compile!                  # AOT-specialise; subsequent valid? calls hit SD
schema.valid?({'name' => 'Bob'}) # specialised path
```

## Numbers

3-way comparison on `benchmark/run.rb`:

| flow | json_schemer | rj_schema (Rust) | arjsv |
|---|---|---|---|
| user object, parsed Hash in   | 1×    | **12×**  | **120×** |
| user object, JSON string in   | 1×    | **18×**  | **80×**  |
| api response x50, parsed Hash in | 1× | **12×**  | **89×**  |
| api response x50, JSON string in | 1× | **13×**  | **60×**  |

(Multiplier = ips relative to json_schemer.)  Even against rj_schema in its
native form (preloaded schema, JSON-string input, RapidJSON-internal
parsing), arjsv stays **4.5–10.7×** faster.  See
[`docs/perf.md`](docs/perf.md) for the full table, the per-property
fstring cache that made this possible, and AOT-vs-interp analysis.
