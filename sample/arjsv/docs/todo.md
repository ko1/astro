# arjsv: TODO

## Tier 2 — common keywords (DONE)

- [x] `pattern` — using Ruby Regexp via rb_reg_match (precompiled at schema
      build).  astrogre integration deferred to a future perf experiment;
      see "Future" section.
- [x] `additionalProperties` (false / sub-schema)
- [x] `patternProperties`
- [x] `propertyNames`
- [x] `minProperties`, `maxProperties`
- [x] `minItems`, `maxItems`, `uniqueItems`
- [x] `items` tuple form + `additionalItems`
- [x] `multipleOf`
- [x] `format` (subset: date, date-time, time, email, uri, ipv4, ipv6, uuid)
- [ ] `Schema#validate(data)` returning error array (json_schemer-compatible
      hashes: data / data_pointer / schema / schema_pointer / type / details)

## Tier 3 — combinators / refs (DONE)

- [x] `allOf` (chain), `anyOf` (chain w/ early-success), `oneOf` (count = 1), `not`
- [x] `if` / `then` / `else`
- [x] `$ref`, `$defs` / `definitions` — internal refs only (`#/$defs/<name>`
      and `#/definitions/<name>`); recursive refs supported via lazy
      `consts`-slot resolution
- [ ] external `$ref` (URI-style, cross-document)
- [ ] 2020-12 draft (separate lowering path keyed off `$schema`)
- [ ] `dependencies` / `dependentSchemas` / `dependentRequired`
- [ ] `contains` / `minContains` / `maxContains` (2019-09+)

## Performance follow-ups (see `docs/perf.md`)

- [x] Cache property-name fstring on `node_property` / `node_required`
      (eliminate `rb_str_new_cstr` per call) — done; ~4.6× on user object,
      see `docs/perf.md` §"Fix 1"
- [ ] Type-check specialisation per common single-type mask (Fixnum-only,
      Float-only, String-only, Hash-only, Array-only)
- [ ] Numeric fast-path in `minimum` / `maximum` (skip `NUM2DBL` for fixnum / flonum)
- [ ] **ASTro framework-level**: embed Ruby VALUE constants directly in
      the SD .rodata via a reload-time fixup pass.  Today `c->consts[idx]`
      adds one load per property check; with a reload-time fixup the SD
      could reference fstring VALUEs directly.  Cross-cutting concern with
      naruby/abruby — likely belongs in `runtime/astro_code_store.{h,c}`.
- [ ] `pattern` via `sample/astrogre`: replace Ruby Regexp with an
      astrogre-compiled regex specialiser, so each schema's pattern gets
      its own SD chain rather than going through Onigmo at runtime.

## Other

- [ ] Schema parsing: accept Symbol keys in input data (not just String)
- [ ] CLI tool: `exe/arjsv` — validate file / stdin against a given schema
- [ ] JSON-Schema-Test-Suite cross-check (subset — only Tier 1 keywords)
- [ ] Memory: `arjsv_dup_key` strdup'd keys are never freed; tie to NODE
      lifetime (free in custom node free hook)
