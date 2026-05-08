# arjsv: TODO

## Done

### Tier 2 — common keywords
- [x] `pattern` (Ruby Regexp via rb_reg_match)
- [x] `additionalProperties` (false / sub-schema / true with key tracking)
- [x] `patternProperties`
- [x] `propertyNames`
- [x] `minProperties` / `maxProperties`
- [x] `minItems` / `maxItems` / `uniqueItems`
- [x] `items` tuple form + `additionalItems`
- [x] `multipleOf`
- [x] `format`: date / date-time / time / duration / email / idn-email /
      uri / uri-reference / iri / iri-reference / json-pointer /
      relative-json-pointer / regex / uri-template / hostname /
      idn-hostname (loose) / ipv4 / ipv6 / uuid

### Tier 3 — combinators / refs
- [x] `allOf` / `anyOf` / `oneOf` / `not`
- [x] `if` / `then` / `else`
- [x] `$ref` to `#`, `#/$defs/<name>`, `#/definitions/<name>`,
      general `#/<json-pointer>`, `<$id>` lookup; recursive refs
- [x] `dependencies` (draft-07) / `dependentRequired` + `dependentSchemas` (2020-12)
- [x] `contains` (+ `minContains` / `maxContains`)
- [x] `prefixItems` (2020-12; normalised to tuple `items`)
- [x] `unevaluatedProperties` / `unevaluatedItems` (2019-09+) — full
      cross-keyword annotation tracking through combinators

### json_schemer-compat API
- [x] `Schema#validate` returning rich error Enumerator (delegates to
      json_schemer when validation fails)
- [x] `Arjsv.valid_schema?` / `Schema#valid_schema?`
- [x] `formats:` constructor option (custom format checkers)
- [x] `insert_property_defaults:` option (mutates data hash with defaults)
- [x] Symbol-key data (transparent dual lookup, String hot path)
- [x] Symbol-key schemas (build-time normalisation)

### draft compatibility
- [x] draft-04 `id` keyword (alongside `$id`)
- [x] draft-07 default
- [x] 2019-09 / 2020-12 (auto-detect via `$schema`)
- [x] Boolean `exclusiveMinimum` / `exclusiveMaximum` (draft-04 form)
- [x] `$schema`-driven assert/annotation defaults (`format`, `content*`)
- [x] `$ref` siblings ignored in draft-07 / honoured in 2019-09+
- [x] RFC 3986 URI base resolution for `$id` / `$ref` (relative refs,
      anchored refs, nested `$id` base changes)
- [x] Quoted-string / domain-literal email format (RFC 5322 / 5321)
- [x] Unicode-aware `idn-email` local-part / domain charset
- [x] `multipleOf` overflow → fall back to Ruby `Rational` for
      precise integer-multiple check (replaces `isfinite` short-circuit)
- [x] `unevaluatedProperties: true` / `unevaluatedItems: true` track
      all keys / items as evaluated (per spec annotation rule)
- [x] `eval_scope` propagates inner annotations to outer scope on
      success (in-place applicator aggregation)

### Performance
- [x] Per-`node_property` / `node_required` fstring cache (eliminate
      `rb_str_new_cstr` per call)
- [x] Type-guard fast path for `type:object` / `type:array` (skip
      RB_TYPE_P in subordinate nodes)
- [x] Format regex shortcut for pure-regex formats (email/uuid/ipv4
      via `node_pattern` instead of Proc-based `node_format`)

## Open

### spec compatibility (deep edges)
- [ ] External `$ref` (HTTP fetching), URN base URIs — out of "no
      external dependency" scope; required to fetch the spec
      meta-schema from `http://json-schema.org/...`
- [ ] `$dynamicRef` / `$dynamicAnchor` full dynamic-scope resolution
      (treated like `$ref` / `$anchor`; static cases work, the cases
      where dynamic scope determines the target do not — 16 fails
      in 2020-12 `dynamicRef.json`)
- [ ] IDNA-2008 punycode validation for `format: hostname` /
      `idn-hostname` (would need libidn / ICU tables)
- [ ] `unevaluatedItems` with `contains`-tracked sparse indices —
      requires switching `c->eval_items` from prefix-count int to a
      sparse-set representation (2 fails in 2020-12)
- [ ] Strict ECMA-262 `pattern` validation (reject `\a` etc.)

### performance follow-ups
- [ ] Conditionalise the `eval_keys` / `eval_items` save/restore in
      property/items nodes — currently unconditional, costs ~25 % on
      schemas without `unevaluated_*`.  Branch on `c->eval_keys != Qnil`
- [ ] Numeric fast path in `minimum` / `maximum` (skip `NUM2DBL` for
      fixnum / flonum)
- [ ] Type-check specialisation per single-type mask
- [ ] **ASTro framework-level**: embed Ruby VALUE constants directly in
      SD `.rodata` via a reload-time fixup pass.  Today `c->consts[idx]`
      adds one load per property check; with a reload-time fixup the SD
      could reference fstring VALUEs as literals.  Cross-cutting with
      naruby / abruby — belongs in `runtime/astro_code_store.{h,c}`
- [ ] `pattern` via `sample/astrogre`: replace Onigmo with an
      astrogre-compiled regex specialiser

### misc
- [ ] CLI tool: `exe/arjsv` — validate file / stdin against a schema
- [ ] Memory: `arjsv_dup_key` strdup'd keys are never freed; tie to
      NODE lifetime
