# arjsv — ASTro Ruby JSON Schema Validator

JSON Schema (draft-07) validator built on the ASTro framework, distributed
as a CRuby C extension.  The schema is lowered to an ASTro AST whose
specialised dispatchers compile down to a tight per-schema validator
function.

Goal: drop-in replacement for `json_schemer.valid?` with substantially
lower per-validation overhead.

## Status

Tier 1 / MVP (see [`docs/done.md`](docs/done.md)).  Tier 2/3 keywords
(`pattern`, `$ref`, `allOf`/`oneOf`/etc.) are listed in
[`docs/todo.md`](docs/todo.md).

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
