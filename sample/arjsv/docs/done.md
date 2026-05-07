# arjsv: implemented features

draft-07, json_schemer-compatible API for `valid?`.

## Public API

```ruby
require 'arjsv'

s = Arjsv.schema({'type' => 'integer', 'minimum' => 0})
s.valid?(42)      # => true / false
s.compile!         # AOT-specialise (otherwise pure interpreter)
```

`Schema#validate(data)` — not yet implemented (Tier 4: rich error
reporting).

## Supported keywords

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
| ref    | `$ref`               | `#/$defs/<name>` and `#/definitions/<name>`; recursive refs supported |
| ref    | `$defs` / `definitions` | each entry compiles as its own SD; cyclic refs resolved via lazy slot |

Boolean schemas: `true` (always valid), `false` (always invalid).
Empty `{}` schema accepted.

## Out of scope

- `Schema#validate(data)` returning rich error list (currently bool-only)
- `$ref` to external documents / non-`$defs` JSON-pointer paths
- 2020-12 draft (separate lowering path keyed off `$schema`)
- `dependencies` / `dependentSchemas` / `dependentRequired` (rare; can chain similarly)
- `contains` / `minContains` / `maxContains` (added in 2019-09)

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
- ~30 node kinds: validate_root, pass, fail, seq, type_check, required,
  property, items_uniform, items_tuple, additional_items, no_additional_items,
  min_items, max_items, unique_items, min_properties, max_properties,
  pattern_property, additional_properties_schema, no_additional_properties,
  property_names, minimum, maximum, multiple_of, min_length, max_length,
  pattern, const, enum, not, if_then_else, any_of, one_of, one_of_step, ref
- Properties / required / enum / pattern_property / etc. are right-recursive
  chains terminated by `pass` / `fail`
- Property names are interned as frozen Ruby Strings in the Schema's `consts`
  Array; runtime lookup is `rb_hash_lookup2(data, c->consts[idx])` — zero
  allocation per validate call.  See `docs/perf.md` §"Fix 1".
- `$ref` uses lazy slot indirection: each `$defs` name reserves a `consts`
  slot at preregistration time, then the body lowering writes the
  validate_root wrapper into the slot.  Recursive `$ref`s pick up *the slot*
  during lowering and read the wrapper from it at runtime.
- `additionalProperties` / `patternProperties` / `propertyNames` use
  `rb_hash_foreach` with a closure struct carrying the target NODE's
  dispatcher (read at runtime).  Their schema bodies are listed as secondary
  entries for compile.
