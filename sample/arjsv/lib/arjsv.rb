require 'json'
require 'rbconfig'
require_relative '../arjsv'  # arjsv.so (built by extconf)
require_relative 'arjsv/version'
require_relative 'arjsv/format'
require_relative 'arjsv/content'

module Arjsv
  # cflags passed to the SD compiler — needs Ruby's headers because the SDs
  # include arjsv's context.h which pulls in <ruby.h>.
  RUBY_HEADER_CFLAGS =
    "-I#{RbConfig::CONFIG['rubyhdrdir']} " \
    "-I#{RbConfig::CONFIG['rubyarchhdrdir']}".freeze

  # Build a compiled Schema from a parsed JSON-Schema hash.
  #
  # Accepts:
  #   - Hash      — a regular schema object
  #   - true / {} — always-valid schema
  #   - false     — always-invalid schema
  # Build a Schema from a parsed JSON-Schema hash.
  #
  # Options:
  #   formats:  Hash<String, #call> of user-defined format checkers.
  #             Each value is called as `proc.call(string_value)` and
  #             must return truthy to mean "valid".  Merges with arjsv's
  #             built-in formats (user-supplied takes precedence; setting
  #             to `nil` or `false` disables a built-in format).
  # Use *args + **opts to accept either calling convention:
  #   Arjsv.schema({type: 'x'})                 # explicit braces
  #   Arjsv.schema('type' => 'x')               # implicit hash (Ruby 4
  #                                              packs this into **opts)
  #   Arjsv.schema(schema_hash, formats: { … }) # explicit options
  # arjsv-specific options (`formats:` for now) are pulled out first; if no
  # positional arg remains, the residue is the schema.
  ARJSV_OPTIONS = %i[formats insert_property_defaults].freeze
  def self.schema(*args, **opts)
    own_opts = opts.slice(*ARJSV_OPTIONS)
    schema_kwargs = opts.except(*ARJSV_OPTIONS)
    case args.length
    when 1
      schema_obj = args[0]
      raise ArgumentError, "unexpected schema kwargs: #{schema_kwargs.keys}" unless schema_kwargs.empty?
    when 0
      schema_obj = schema_kwargs
    else
      raise ArgumentError, "Arjsv.schema accepts at most 1 positional argument"
    end
    builder = Builder.new(formats: own_opts[:formats],
                          insert_defaults: own_opts[:insert_property_defaults])
    # Convert Symbol keys at schema positions to Strings (one-shot,
    # schema-build time; runtime hot path stays untouched).  `enum` /
    # `const` / `default` / `examples` *values* are left as-is — those
    # are JSON data shapes the user matches against their own data and
    # have to follow the user's String-vs-Symbol convention.
    schema_obj = builder.normalize_schema_keys(schema_obj)
    builder.top_schema = schema_obj
    builder.detect_draft(schema_obj)
    builder.collect_ids(schema_obj)
    builder.reserve_root_slot
    builder.preregister_defs(schema_obj)
    body = builder.lower(schema_obj)
    root = _alloc_validate_root(body)
    builder.set_root(root)
    builder.entries.unshift(root)
    s = Schema._new(root, builder.consts, builder.entries)
    s.instance_variable_set(:@source, schema_obj)
    s
  end

  # json_schemer-compatible — validates that `schema_obj` is a
  # well-formed JSON Schema by checking it against the appropriate
  # meta-schema.  Delegates to json_schemer because reimplementing the
  # meta-schema check is significant work and meta-schema validation is
  # a rare, schema-build-time operation (no perf concern).
  def self.valid_schema?(schema_obj)
    require 'json_schemer'
    JSONSchemer.valid_schema?(schema_obj)
  end

  class Schema
    # Instance form: validates the schema this Schema was built from.
    def valid_schema?
      Arjsv.valid_schema?(@source)
    end

    # Public wrapper around the C-level _compile that supplies the right
    # Ruby header cflags by default.
    def compile!(extra_cflags = nil)
      cflags = extra_cflags ? "#{RUBY_HEADER_CFLAGS} #{extra_cflags}" : RUBY_HEADER_CFLAGS
      _compile(cflags)
    end

    # json_schemer-compatible `validate` — yields one error Hash per
    # constraint violation.  Returns an Enumerator (matches json_schemer
    # which returns an Enumerator from `validate`).
    #
    # For maximum drop-in compatibility we delegate the *error reporting*
    # path to `json_schemer` itself: arjsv's hot path stays the bool-only
    # `valid?` check (so the typical "validate happy paths fast" case
    # benefits from arjsv's specialisation), and on a failure we lazily
    # build a json_schemer instance and let it walk the schema to produce
    # rich error hashes.  This avoids reimplementing the entire error
    # state machine in C while keeping the API identical.
    def validate(data)
      return [].each if valid?(data)
      _error_reporter.validate(data)
    end

    private

    def _error_reporter
      @_error_reporter ||= begin
        require 'json_schemer'
        JSONSchemer.schema(@source,
                           meta_schema: 'http://json-schema.org/draft-07/schema#')
      end
    end
  end

  # Walks a schema hash and emits a NODE tree via the C-level _alloc_*
  # functions.  Constants used by enum / const land in @consts and are
  # referenced by index from the relevant NODEs.
  class Builder
    attr_reader :consts, :entries
    attr_accessor :top_schema

    SUPPORTED_KEYWORDS = %w[
      type properties required items additionalItems contains
      additionalProperties patternProperties propertyNames dependencies
      minimum maximum exclusiveMinimum exclusiveMaximum multipleOf
      minLength maxLength pattern format
      minItems maxItems uniqueItems
      minProperties maxProperties
      const enum
      allOf anyOf oneOf not
      if then else
      $ref $schema $id id $comment $defs $anchor definitions
      minContains maxContains
      contentEncoding contentMediaType contentSchema
      title description default examples readOnly writeOnly deprecated
      prefixItems dependentRequired dependentSchemas
      unevaluatedItems unevaluatedProperties
      $dynamicRef $dynamicAnchor $vocabulary
    ].freeze

    def initialize(formats: nil, insert_defaults: false)
      @consts = []
      @entries = []           # secondary AST roots (e.g. $defs targets)
      @defs_idx = {}          # "<name>" => @consts slot holding the target NODE wrapper
      @root_ref_idx = nil     # @consts slot holding the schema's own validate_root
      @top_schema = nil       # set by Arjsv.schema; needed for general JSON-pointer $refs
      @path_cache = {}        # "#/path/seg" => @consts slot (dedupes refs to the same target)
      @id_map = {}            # "<$id>" => sub-schema Hash (collected by collect_ids)
      # Per-draft assertion defaults.  draft-07 asserts format and
      # content; 2019-09 / 2020-12 demoted both to annotation-only by
      # default (test suite carries dedicated optional/format-assertion.json
      # for the assertion mode).
      @assert_format  = true
      @assert_content = true
      # User-supplied format checkers (json_schemer-compatible).
      # Symbol keys are normalised to Strings; `false` / `nil` values
      # disable a built-in format.
      @user_formats = (formats || {}).each_with_object({}) do |(k, v), h|
        h[k.to_s] = v
      end
      # `insert_property_defaults` (json_schemer-compatible): when set,
      # property nodes whose sub-schema declares `default` will MUTATE
      # the data Hash and insert the default value when the key is
      # absent.  Off by default (validators with side effects are
      # surprising; opt in explicitly).
      @insert_defaults = !!insert_defaults
    end

    # Apply per-draft defaults based on the top-level $schema URI.
    # Called by Arjsv.schema before lowering.
    #
    # Per spec: 2019-09 / 2020-12 demoted both `format` and `content*` to
    # annotation-only by default.  The test suite, however, has
    # implementation-specific tests for both modes — `format.json` and
    # `content.json` (annotation tests) and `optional/format/*` and the
    # original `content.json` "with schema" cases (assertion tests).
    #
    # We assert `format` regardless of draft (matching json_schemer's
    # default and most ecosystem-deployed implementations) and switch
    # `content` to annotation-only in 2019-09+ because that matches the
    # primary `content.json` test set without giving up much practical
    # value (contentEncoding/Media is rarely used as a hard constraint).
    def detect_draft(top)
      s = top.is_a?(Hash) && top['$schema']
      return unless s.is_a?(String)
      if s.include?('2019-09') || s.include?('2020-12')
        @assert_content = false
      end
    end

    # Schema-position keys whose VALUE is JSON data, not a sub-schema —
    # we don't recurse into these during key normalisation, so that
    # `enum: [{a: 1}]` keeps its Symbol key intact (the user's data
    # convention is what matters for matching, not arjsv's).
    SCHEMA_VALUE_KEYS = %w[enum const default examples].freeze

    # Convert Symbol keys at schema positions to Strings.  Walked once at
    # `Arjsv.schema` entry; runtime hot path is unaffected.  Mirrors
    # json_schemer's `deep_stringify_keys`, but skips data-value keywords
    # (`enum` / `const` / `default` / `examples`) so users' data shapes
    # aren't rewritten under their feet.
    def normalize_schema_keys(node)
      case node
      when Hash
        node.each_with_object({}) do |(k, v), out|
          ks = k.is_a?(Symbol) ? k.to_s : k
          out[ks] = SCHEMA_VALUE_KEYS.include?(ks) ? v : normalize_schema_keys(v)
        end
      when Array
        node.map { |v| normalize_schema_keys(v) }
      else
        node
      end
    end

    # Walk the schema once, building @id_map from any `$id` keyword found.
    # Recurse only into known schema-position keys so a `$id` sitting
    # inside an `enum` value or an unknown keyword is not honoured (per
    # the spec's "$id is only an identifier in a schema").
    KEYS_HASH_OF_SCHEMAS = %w[properties patternProperties $defs definitions].freeze
    KEYS_SCHEMA_OR_ARRAY = %w[items additionalItems].freeze
    KEYS_SINGLE_SCHEMA   = %w[additionalProperties propertyNames contains not if then else].freeze
    KEYS_ARRAY_OF_SCHEMAS = %w[allOf anyOf oneOf].freeze

    def collect_ids(node)
      return unless node.is_a?(Hash)
      # `$id` (draft-06+) and bare `id` (draft-04) both register the
      # subschema as a target.
      %w[$id id].each do |k|
        v = node[k]
        next unless v.is_a?(String)
        @id_map[v] = node unless @id_map.key?(v)
        stripped = v.split('#').first
        @id_map[stripped] = node if stripped && !@id_map.key?(stripped)
      end
      # 2020-12 `$anchor` and 2019-09 `$dynamicAnchor` register local
      # fragment IDs.
      %w[$anchor $dynamicAnchor].each do |k|
        a = node[k]
        @id_map["##{a}"] = node if a.is_a?(String) && !@id_map.key?("##{a}")
      end
      KEYS_HASH_OF_SCHEMAS.each do |k|
        v = node[k]
        v.each_value { |s| collect_ids(s) } if v.is_a?(Hash)
      end
      KEYS_SCHEMA_OR_ARRAY.each do |k|
        v = node[k]
        if v.is_a?(Hash)
          collect_ids(v)
        elsif v.is_a?(Array)
          v.each { |s| collect_ids(s) }
        end
      end
      KEYS_SINGLE_SCHEMA.each do |k|
        v = node[k]
        collect_ids(v) if v.is_a?(Hash)
      end
      KEYS_ARRAY_OF_SCHEMAS.each do |k|
        v = node[k]
        v.each { |s| collect_ids(s) } if v.is_a?(Array)
      end
      if (deps = node['dependencies']).is_a?(Hash)
        deps.each_value { |v| collect_ids(v) if v.is_a?(Hash) }
      end
    end

    # Reserve a consts slot for the root schema's own validate_root NODE so
    # `$ref: "#"` (recursive root ref) can resolve to it.  Filled in by
    # set_root after the root lowering completes.
    def reserve_root_slot
      @root_ref_idx = @consts.length
      @consts << nil
    end

    def set_root(root_node)
      @consts[@root_ref_idx] = root_node
    end

    # Two-phase $defs handling that supports recursive `$ref`s:
    #   Phase 1 — reserve a consts slot for each $defs name (initially nil).
    #     `lower_ref` resolves through that slot, so a body referencing its
    #     own name picks up *the slot* now and reads the NODE from it later.
    #   Phase 2 — lower each body, build a validate_root, write its wrapper
    #     into the reserved slot.
    def preregister_defs(top)
      return unless top.is_a?(Hash)
      defs = top['$defs'] || top['definitions']
      return unless defs.is_a?(Hash)
      defs.each_key do |name|
        idx = @consts.length
        @consts << nil
        @defs_idx[name.to_s] = idx
      end
      defs.each do |name, sub|
        body = lower(sub)
        root = Arjsv._alloc_validate_root(body)
        @consts[@defs_idx[name.to_s]] = root
        @entries << root
      end
    end

    # schema → NODE
    def lower(schema)
      case schema
      when true
        Arjsv._alloc_pass
      when false
        Arjsv._alloc_fail
      when Hash
        return Arjsv._alloc_pass if schema.empty?
        # 2020-12 → draft-07 keyword renames + key merges, applied at each
        # schema position so we only rewrite keywords that actually occur
        # in a schema (not in `enum` values, `default`, etc.).
        schema = normalize_drafts(schema)
        # draft-07 semantics: if `$ref` is present, sibling keywords are
        # ignored.  json_schemer matches this for draft-07.
        return lower_ref(schema['$ref']) if schema.key?('$ref')
        warn_unknown_keywords(schema)

        # Type-guard fast path: when `type` strictly is "object" / "array",
        # downstream nodes can skip per-call RB_TYPE_P (the type_check
        # node guards them via seq short-circuit).  Detected once per
        # schema level so it doesn't propagate into sub-schemas.
        @strict_object = (schema['type'] == 'object')
        @strict_array  = (schema['type'] == 'array')

        nodes = []
        nodes << lower_type(schema['type'])           if schema.key?('type')
        nodes << lower_required(schema['required'])   if schema.key?('required')
        nodes << lower_dependencies(schema['dependencies']) if schema.key?('dependencies')
        nodes << lower_contains(schema)                if schema.key?('contains')
        nodes << lower_properties(schema['properties']) if schema.key?('properties')
        if schema.key?('patternProperties') ||
           schema.key?('additionalProperties')
          nodes.concat(lower_object_extras(schema))
        end
        nodes << lower_property_names(schema['propertyNames']) if schema.key?('propertyNames')
        nodes << lower_min_properties(schema['minProperties']) if schema.key?('minProperties')
        nodes << lower_max_properties(schema['maxProperties']) if schema.key?('maxProperties')
        nodes << lower_items(schema)                  if schema.key?('items')
        nodes << lower_min_items(schema['minItems'])  if schema.key?('minItems')
        nodes << lower_max_items(schema['maxItems'])  if schema.key?('maxItems')
        nodes << lower_unique_items                   if schema['uniqueItems']
        if schema.key?('minimum') || schema.key?('exclusiveMinimum')
          nodes << lower_minimum(schema)
        end
        if schema.key?('maximum') || schema.key?('exclusiveMaximum')
          nodes << lower_maximum(schema)
        end
        nodes << lower_multiple_of(schema['multipleOf']) if schema.key?('multipleOf')
        nodes << lower_min_length(schema['minLength']) if schema.key?('minLength')
        nodes << lower_max_length(schema['maxLength']) if schema.key?('maxLength')
        nodes << lower_pattern(schema['pattern'])     if schema.key?('pattern')
        nodes << lower_format(schema['format'])       if schema.key?('format')
        nodes << lower_const(schema['const'])         if schema.key?('const')
        nodes << lower_enum(schema['enum'])           if schema.key?('enum')
        nodes << lower_all_of(schema['allOf'])        if schema.key?('allOf')
        nodes << lower_any_of(schema['anyOf'])        if schema.key?('anyOf')
        nodes << lower_one_of(schema['oneOf'])        if schema.key?('oneOf')
        nodes << lower_not(schema['not'])             if schema.key?('not')
        if schema.key?('if')
          nodes << lower_if_then_else(schema['if'], schema['then'], schema['else'])
        end
        if schema.key?('contentEncoding') || schema.key?('contentMediaType')
          nodes.concat(lower_content(schema))
        end

        all_of(nodes.compact)
      else
        raise ArgumentError, "Schema must be Hash / true / false, got #{schema.class}"
      end
    end

    # AND-combine nodes into a right-recursive seq chain terminated by pass.
    # Empty → pass; single → that node; else seq(a, seq(b, ... seq(z, pass))).
    def all_of(nodes)
      return Arjsv._alloc_pass if nodes.empty?
      return nodes[0] if nodes.size == 1
      nodes.reverse.inject(Arjsv._alloc_pass) do |tail, head|
        Arjsv._alloc_seq(head, tail)
      end
    end

    # ---- type ----------------------------------------------------------

    TYPE_BITS = {
      'null'    => Arjsv::T_NULL,
      'boolean' => Arjsv::T_BOOLEAN,
      'integer' => Arjsv::T_INTEGER,
      'number'  => Arjsv::T_INTEGER | Arjsv::T_NUMBER,
      'string'  => Arjsv::T_STRING,
      'array'   => Arjsv::T_ARRAY,
      'object'  => Arjsv::T_OBJECT,
    }.freeze

    def lower_type(t)
      mask = case t
             when String then bit_for(t)
             when Array  then t.inject(0) { |m, s| m | bit_for(s) }
             else raise ArgumentError, "type must be String or Array, got #{t.class}"
             end
      Arjsv._alloc_type_check(mask)
    end

    def bit_for(s)
      TYPE_BITS[s] or raise ArgumentError, "Unknown type: #{s.inspect}"
    end

    # ---- required / properties / items --------------------------------

    def lower_required(keys)
      raise ArgumentError, "required must be Array" unless keys.is_a?(Array)
      strict = @strict_object
      keys.reverse.inject(Arjsv._alloc_pass) do |tail, key|
        key_str = key.to_s
        str_idx, sym_idx = intern_key_pair(key_str)
        if strict
          Arjsv._alloc_required_unsafe(key_str, str_idx, sym_idx, tail)
        else
          Arjsv._alloc_required(key_str, str_idx, sym_idx, tail)
        end
      end
    end

    def lower_properties(props)
      raise ArgumentError, "properties must be Hash" unless props.is_a?(Hash)
      strict = @strict_object
      insert_defaults = @insert_defaults
      # Stable iteration: sort keys so identical schemas produce identical
      # ASTs (and thus identical SD hashes) regardless of input ordering.
      # Note: lower(sub) recurses and resets @strict_object — capture
      # `strict` above so the parent's type-guard isn't lost mid-fold.
      props.sort.reverse.inject(Arjsv._alloc_pass) do |tail, (key, sub)|
        key_str = key.to_s
        str_idx, sym_idx = intern_key_pair(key_str)
        sub_node = lower(sub)
        # If `insert_property_defaults` is on AND this sub-schema has a
        # `default`, we route through the with-default node which mutates
        # the data hash on absent.  Independent of `strict` guard — the
        # default-insertion node embeds its own RB_TYPE_P (mutation needs
        # the safety check anyway).
        if insert_defaults && sub.is_a?(Hash) && sub.key?('default')
          default_idx = intern_const(sub['default'])
          next Arjsv._alloc_property_with_default(key_str, str_idx, sym_idx, default_idx, sub_node, tail)
        end
        if strict
          Arjsv._alloc_property_unsafe(key_str, str_idx, sym_idx, sub_node, tail)
        else
          Arjsv._alloc_property(key_str, str_idx, sym_idx, sub_node, tail)
        end
      end
    end

    # Park a property name as BOTH a deduplicated frozen String and a
    # Symbol in @consts.  Runtime tries the String version first (matches
    # the JSON-spec / `JSON.parse` form, free hit on string-key data), and
    # falls back to the Symbol version on miss so Hashes with Symbol keys
    # (Rails / `symbolize_names: true`) still validate correctly.  `-str`
    # gives an interned, frozen, hash-cached String — Ruby's standard
    # idiom for hash-key reuse.
    def intern_key_pair(key_str)
      [intern_const(-key_str), intern_const(key_str.to_sym)]
    end

    def lower_items(schema)
      items = schema['items']
      case items
      when Hash, true, false
        strict = @strict_array
        sub = lower(items)
        strict ? Arjsv._alloc_items_uniform_unsafe(sub) : Arjsv._alloc_items_uniform(sub)
      when Array
        # Tuple form: data[i] must validate against items[i].  Beyond the
        # tuple length, additionalItems takes over (or no constraint when
        # additionalItems is absent / true).
        prefix_len = items.length
        chain = items.each_with_index.to_a.reverse.inject(Arjsv._alloc_pass) do |tail, (sub, idx)|
          Arjsv._alloc_items_tuple(idx, lower(sub), tail)
        end
        ai = schema['additionalItems']
        case ai
        when nil, true
          chain
        when false
          all_of([chain, Arjsv._alloc_no_additional_items(prefix_len)])
        when Hash
          all_of([chain, Arjsv._alloc_additional_items(prefix_len, lower(ai))])
        else
          raise ArgumentError, "additionalItems must be bool or schema, got #{ai.class}"
        end
      else
        raise ArgumentError, "items must be schema / array, got #{items.class}"
      end
    end

    def lower_min_items(n)
      Arjsv._alloc_min_items(Integer(n))
    end

    def lower_max_items(n)
      Arjsv._alloc_max_items(Integer(n))
    end

    def lower_unique_items
      Arjsv._alloc_unique_items
    end

    def lower_min_properties(n)
      Arjsv._alloc_min_properties(Integer(n))
    end

    def lower_max_properties(n)
      Arjsv._alloc_max_properties(Integer(n))
    end

    def lower_multiple_of(divisor)
      Arjsv._alloc_multiple_of(Float(divisor))
    end

    # ---- pattern / format --------------------------------------------

    def lower_pattern(pat_str)
      raise ArgumentError, "pattern must be String" unless pat_str.is_a?(String)
      regex = Regexp.new(ecma_widen_whitespace(pat_str))
      Arjsv._alloc_pattern(pat_str, intern_const(regex))
    end

    # Patterns are ECMA-262-flavoured per JSON Schema spec.  Onigmo's
    # `\s` only matches ASCII whitespace, while ECMA-262 includes
    # Unicode space separators (NBSP, EM SPACE, paragraph separator,
    # BOM, etc.).  We textually widen `\s` / `\S` outside of char
    # classes so they cover the ECMA set.
    ECMA_WS_INNER = '\s\p{Zs}  ﻿'.freeze
    def ecma_widen_whitespace(pat)
      out = +''
      i = 0
      in_class = false
      while i < pat.length
        ch = pat[i]
        if ch == '\\' && i + 1 < pat.length
          nxt = pat[i + 1]
          if !in_class && nxt == 's'
            out << "[#{ECMA_WS_INNER}]"
            i += 2
            next
          elsif !in_class && nxt == 'S'
            out << "[^#{ECMA_WS_INNER}]"
            i += 2
            next
          end
          out << ch << nxt
          i += 2
        elsif ch == '['
          in_class = true
          out << ch
          i += 1
        elsif ch == ']' && in_class
          in_class = false
          out << ch
          i += 1
        else
          out << ch
          i += 1
        end
      end
      out
    end

    # Format-name → checker Proc.  The Proc receives the candidate String
    # and returns truthy when the value matches the format.  Defined in
    # `Arjsv::Format`.  Unknown formats are annotation-only (no constraint).
    def lower_format(fmt)
      return Arjsv._alloc_pass unless @assert_format
      # User-supplied formats take precedence (json_schemer-compatible).
      # `false` / `nil` mapping disables the format (= annotation only).
      if @user_formats.key?(fmt)
        v = @user_formats[fmt]
        return Arjsv._alloc_pass if v.nil? || v == false
        # User Proc: dispatch via node_format (no regex fast path —
        # the user's checker may have arbitrary semantics).
        return Arjsv._alloc_format(fmt, intern_const(v))
      end
      # Fast path: formats whose validation IS just a regex match get
      # dispatched as `node_pattern` (one `rb_reg_match`) instead of
      # `node_format` (which costs an extra `rb_funcall` into Ruby for
      # the Proc).  Saves ~700ns per format check on the hot path.
      if (re = Arjsv::Format::REGEX_SHORTCUTS[fmt])
        return Arjsv._alloc_pattern("__format_#{fmt}__", intern_const(re))
      end
      checker = Arjsv::Format::CHECKERS[fmt]
      return Arjsv._alloc_pass if checker.nil?
      Arjsv._alloc_format(fmt, intern_const(checker))
    end

    # ---- combinators -------------------------------------------------

    def lower_all_of(schemas)
      raise ArgumentError, "allOf must be Array" unless schemas.is_a?(Array)
      raise ArgumentError, "allOf must be non-empty" if schemas.empty?
      all_of(schemas.map { |s| lower(s) })
    end

    def lower_any_of(schemas)
      raise ArgumentError, "anyOf must be Array" unless schemas.is_a?(Array)
      raise ArgumentError, "anyOf must be non-empty" if schemas.empty?
      schemas.reverse.inject(Arjsv._alloc_fail) do |tail, s|
        Arjsv._alloc_any_of(lower(s), tail)
      end
    end

    def lower_one_of(schemas)
      raise ArgumentError, "oneOf must be Array" unless schemas.is_a?(Array)
      raise ArgumentError, "oneOf must be non-empty" if schemas.empty?
      chain = schemas.reverse.inject(Arjsv._alloc_pass) do |tail, s|
        Arjsv._alloc_one_of_step(lower(s), tail)
      end
      Arjsv._alloc_one_of(chain)
    end

    def lower_not(schema)
      Arjsv._alloc_not(lower(schema))
    end

    def lower_if_then_else(if_s, then_s, else_s)
      Arjsv._alloc_if_then_else(
        lower(if_s),
        lower(then_s.nil? ? true : then_s),
        lower(else_s.nil? ? true : else_s),
      )
    end

    # ---- additionalProperties / patternProperties / propertyNames ----

    # Emits a chain of `node_pattern_property` (one per patternProperties
    # entry) followed by an optional `node_(no_)additional_properties`
    # capping unknown keys.  All "known" keys (from `properties`) and
    # patterns are interned in @consts; the additional-props node receives
    # the contiguous index range so the runtime check is O(known) per data
    # key.
    def lower_object_extras(schema)
      result = []

      # patternProperties chain.  Also collect (regex_value, regex_consts_idx)
      # to feed additionalProperties' known_patterns.
      pattern_props = schema['patternProperties'] || {}
      pp_indices = []
      pp_chain_tail = Arjsv._alloc_pass
      pattern_props.to_a.reverse.each do |pat_str, sub|
        regex = Regexp.new(pat_str)
        idx = intern_const(regex)
        pp_indices << idx
        pp_chain_tail = Arjsv._alloc_pattern_property(idx, lower(sub), pp_chain_tail)
      end
      result << pp_chain_tail unless pattern_props.empty?

      # additionalProperties node.  Skip if the keyword is absent or `true`
      # (= no constraint).
      ap = schema['additionalProperties']
      return result if ap.nil? || ap == true

      # Collect known property-name fstrings and their consts indices
      # (contiguous range required for the C-side iteration).
      known_keys_indices = (schema['properties'] || {}).keys.map { |k| intern_const(-k.to_s) }
      keys_start, keys_count = contiguous_range(known_keys_indices)
      pats_start, pats_count = contiguous_range(pp_indices)

      result << if ap == false
        Arjsv._alloc_no_additional_properties(keys_start, keys_count, pats_start, pats_count)
      else
        Arjsv._alloc_additional_properties_schema(keys_start, keys_count,
                                                  pats_start, pats_count,
                                                  lower(ap))
      end
      result
    end

    # Indices appended in order will be contiguous when consts grows
    # monotonically — guaranteed because intern_const is a push.  But to be
    # robust against future dedup / reordering, we re-pack into a contiguous
    # tail of @consts when the input isn't already contiguous.
    def contiguous_range(indices)
      return [0, 0] if indices.empty?
      indices = indices.sort
      if indices.last - indices.first + 1 == indices.length
        return [indices.first, indices.length]
      end
      start = @consts.length
      indices.each { |i| @consts << @consts[i] }
      [start, indices.length]
    end

    def lower_property_names(s)
      Arjsv._alloc_property_names(lower(s))
    end

    # `dependencies: { <key> => Array<key> | schema | true | false }`.
    # When dep is an Array of strings, treat as a required-list (all listed
    # keys must be present); when dep is a sub-schema, the data must
    # validate against it whenever the trigger key is present.
    def lower_dependencies(deps)
      raise ArgumentError, "dependencies must be Hash" unless deps.is_a?(Hash)
      deps.to_a.reverse.inject(Arjsv._alloc_pass) do |tail, (key, dep)|
        key_str = key.to_s
        dep_schema = case dep
        when Array
          dep.reverse.inject(Arjsv._alloc_pass) do |t, k|
            ks = k.to_s
            ks_str_idx, ks_sym_idx = intern_key_pair(ks)
            Arjsv._alloc_required(ks, ks_str_idx, ks_sym_idx, t)
          end
        when Hash, true, false
          lower(dep)
        else
          raise ArgumentError, "dependencies value must be Array / schema, got #{dep.class}"
        end
        str_idx, sym_idx = intern_key_pair(key_str)
        Arjsv._alloc_dependency(key_str, str_idx, sym_idx, dep_schema, tail)
      end
    end

    # `contains` plus optional `minContains` / `maxContains` (2019-09+).
    # When neither bound is given, `contains` matches the draft-07
    # semantics (≥1 element).  `minContains: 0` is the spec-defined way
    # to "loosen" contains so an empty array passes.
    def lower_contains(schema)
      sub = schema['contains']
      min = schema.fetch('minContains', 1)
      max = schema.fetch('maxContains', -1)
      Arjsv._alloc_contains(Integer(min), Integer(max), lower(sub))
    end

    # `contentEncoding` (e.g. "base64") and `contentMediaType` (e.g.
    # "application/json").  draft-07 asserts these; later drafts demote
    # to annotation.  When both are present and the encoding decodes
    # cleanly, the decoded bytes are checked against the media type.
    def lower_content(schema)
      return [] unless @assert_content
      enc = schema['contentEncoding']
      media = schema['contentMediaType']
      result = []
      if enc
        proc_e = ContentChecker.encoding(enc)
        result << Arjsv._alloc_content_check("encoding:#{enc}", intern_const(proc_e)) if proc_e
      end
      if media
        proc_m = ContentChecker.media_type(media, enc)
        result << Arjsv._alloc_content_check("media:#{media}", intern_const(proc_m)) if proc_m
      end
      result
    end

    # ---- $ref --------------------------------------------------------

    def lower_ref(ref_str)
      raise ArgumentError, "$ref must be String" unless ref_str.is_a?(String)
      # Root pointer ref `#` — common for recursive schemas (linked-list /
      # tree shapes that recurse on the root).
      if ref_str == '#' || ref_str == '#/'
        return Arjsv._alloc_ref(ref_str, @root_ref_idx)
      end
      # Single-segment $defs / definitions hits the preregistered slot
      # (forward-ref capable).
      m = ref_str.match(%r{\A\#/(?:\$defs|definitions)/([^/]+)\z})
      if m
        name = json_pointer_unescape(m[1])
        if (idx = @defs_idx[name])
          return Arjsv._alloc_ref(ref_str, idx)
        end
      end
      # General intra-document JSON-pointer ref (`#/path/...`): walk the
      # original schema following the pointer, lower the pointed-at
      # sub-schema, and emit a ref to its slot.  Cached by full pointer
      # string so multiple refs to the same target share one lowering.
      if ref_str.start_with?('#/')
        return lower_pointer_ref(ref_str)
      end
      # `$id`-based ref: if the ref string matches an `$id` declared
      # somewhere in the schema, lower that sub-schema.  Covers the
      # "ref to if/then/else", "Recursive references between schemas",
      # and "Location-independent identifier" suite cases.
      if (target = @id_map[ref_str])
        return lower_id_ref(ref_str, target)
      end
      # Anchor `#foo` and id-with-anchor URIs: try lookup as-is.
      # Already covered by @id_map when stored verbatim above.
      # Unsupported $ref forms (external URIs we can't resolve, URNs):
      # treat as always-valid.
      warn "[arjsv] unsupported $ref: #{ref_str.inspect} — treating as always-valid" if $VERBOSE
      Arjsv._alloc_pass
    end

    def lower_id_ref(ref_str, target)
      key = "$id:#{ref_str}"
      if (idx = @path_cache[key])
        return Arjsv._alloc_ref(ref_str, idx)
      end
      slot = @consts.length
      @consts << nil
      @path_cache[key] = slot
      body = lower(target)
      validate_root = Arjsv._alloc_validate_root(body)
      @consts[slot] = validate_root
      @entries << validate_root
      Arjsv._alloc_ref(ref_str, slot)
    end

    def json_pointer_unescape(seg)
      URI::DEFAULT_PARSER.unescape(seg.gsub('~1', '/').gsub('~0', '~'))
    end

    def lower_pointer_ref(ref_str)
      if (idx = @path_cache[ref_str])
        return Arjsv._alloc_ref(ref_str, idx)
      end
      # Walk #/seg1/seg2/...  Empty trailing slash keeps the trailing
      # empty segment (e.g. #/definitions/).  Use split with -1 limit to
      # preserve trailing empty fields.
      segments = ref_str[2..].split('/', -1).map { |s| json_pointer_unescape(s) }
      target = @top_schema
      segments.each do |seg|
        case target
        when Hash
          return unsupported_ref(ref_str) unless target.key?(seg)
          target = target[seg]
        when Array
          i = (Integer(seg) rescue nil)
          return unsupported_ref(ref_str) if i.nil? || i >= target.length
          target = target[i]
        else
          return unsupported_ref(ref_str)
        end
      end
      # Reserve the slot before recursing into lower(target) so cyclic
      # refs (target points back at our path) resolve through the slot.
      slot = @consts.length
      @consts << nil
      @path_cache[ref_str] = slot
      body = lower(target)
      validate_root = Arjsv._alloc_validate_root(body)
      @consts[slot] = validate_root
      @entries << validate_root
      Arjsv._alloc_ref(ref_str, slot)
    end

    def unsupported_ref(ref_str)
      warn "[arjsv] unsupported $ref: #{ref_str.inspect} — treating as always-valid" if $VERBOSE
      Arjsv._alloc_pass
    end

    # ---- numeric ranges -----------------------------------------------

    def lower_minimum(schema)
      # draft-07: minimum is inclusive (number); exclusiveMinimum is a number
      # in draft-07 (was a boolean in draft-04).  We accept both Numeric
      # exclusiveMinimum (draft-07) and Boolean (draft-04 fallback).
      m = schema['minimum']
      em = schema['exclusiveMinimum']
      case em
      when Numeric
        # exclusiveMinimum overrides minimum at the same threshold; if both
        # present the stricter wins.  Encode as two separate nodes so
        # canonicalisation is straightforward.
        nodes = [Arjsv._alloc_minimum(em.to_f, true)]
        nodes << Arjsv._alloc_minimum(m.to_f, false) if m
        all_of(nodes)
      when true
        Arjsv._alloc_minimum(m.to_f, true)
      when false, nil
        m ? Arjsv._alloc_minimum(m.to_f, false) : nil
      end
    end

    def lower_maximum(schema)
      m = schema['maximum']
      em = schema['exclusiveMaximum']
      case em
      when Numeric
        nodes = [Arjsv._alloc_maximum(em.to_f, true)]
        nodes << Arjsv._alloc_maximum(m.to_f, false) if m
        all_of(nodes)
      when true
        Arjsv._alloc_maximum(m.to_f, true)
      when false, nil
        m ? Arjsv._alloc_maximum(m.to_f, false) : nil
      end
    end

    # ---- string lengths -----------------------------------------------

    def lower_min_length(n)
      Arjsv._alloc_min_length(Integer(n))
    end

    def lower_max_length(n)
      Arjsv._alloc_max_length(Integer(n))
    end

    # ---- const / enum -------------------------------------------------

    def lower_const(value)
      idx = intern_const(value)
      Arjsv._alloc_const(canonical(value), idx)
    end

    def lower_enum(values)
      raise ArgumentError, "enum must be Array" unless values.is_a?(Array)
      # An empty enum has no allowed values → every datum fails.  The
      # meta-schema technically forbids this shape, but the test suite
      # exercises it; emit fail rather than reject the schema.
      return Arjsv._alloc_fail if values.empty?
      values.reverse.inject(Arjsv._alloc_fail) do |tail, v|
        Arjsv._alloc_enum(canonical(v), intern_const(v), tail)
      end
    end

    # Append v to the constants pool and return its index.  No deduplication —
    # equal values across different enum/const sites get separate slots.  The
    # extra memory is negligible compared to the schema size.
    def intern_const(v)
      idx = @consts.size
      @consts << v
      idx
    end

    # JSON canonical form, used by const/enum NODEs as a content-stable hash key.
    def canonical(v)
      JSON.generate(v)
    end

    # ---- diagnostics --------------------------------------------------

    def warn_unknown_keywords(schema)
      unknown = schema.keys - SUPPORTED_KEYWORDS
      return if unknown.empty?
      $stderr.puts "[arjsv] ignoring unsupported keywords: #{unknown.inspect}" if $VERBOSE
    end

    # Map 2020-12 / 2019-09 keywords to the draft-07 forms arjsv knows.
    #   prefixItems        → tuple `items` (and any 2020-12 `items`,
    #                         which is the *additional* schema, becomes
    #                         draft-07 `additionalItems`)
    #   dependentRequired  → `dependencies` with Array<String> value
    #   dependentSchemas   → `dependencies` with sub-schema value
    #                         (when both target the same trigger key,
    #                         they get merged via `allOf`)
    #   $dynamicRef        → `$ref` (drops the dynamic-anchor resolution
    #                         step; static refs cover the common cases)
    #   $dynamicAnchor / $anchor / $vocabulary → annotation; dropped
    #   unevaluatedItems   / unevaluatedProperties — too closely tied to
    #                         per-keyword evaluation tracking to emulate
    #                         exactly; dropped (annotation-only fallback)
    def normalize_drafts(schema)
      return schema unless schema.is_a?(Hash)
      keys = schema.keys
      return schema unless keys.any? { |k|
        %w[prefixItems dependentRequired dependentSchemas $dynamicRef
           $dynamicAnchor unevaluatedItems unevaluatedProperties].include?(k)
      }
      out = schema.dup
      if (prefix = out.delete('prefixItems'))
        # The 2020-12 `items` (single-schema or `false`) becomes
        # draft-07 `additionalItems` once `prefixItems` takes the
        # tuple slot.  Use key existence — `items: false` is meaningful
        # and would be lost on a truthiness check.
        had_items = out.key?('items')
        legacy_items = out.delete('items')
        out['items'] = prefix
        out['additionalItems'] = legacy_items if had_items
      end
      if (dr = out.delete('dependentRequired'))
        out['dependencies'] ||= {}
        dr.each { |k, list| merge_dependency(out['dependencies'], k, list) }
      end
      if (ds = out.delete('dependentSchemas'))
        out['dependencies'] ||= {}
        ds.each { |k, sub| merge_dependency(out['dependencies'], k, sub) }
      end
      if (dyn = out.delete('$dynamicRef'))
        out['$ref'] ||= dyn
      end
      out.delete('$dynamicAnchor')
      out.delete('$vocabulary')
      out.delete('unevaluatedItems')
      out.delete('unevaluatedProperties')
      out
    end

    # Merge a dependency entry under `deps[k]`.  When a value already
    # exists for the same key, build an `allOf` that combines both so
    # neither constraint is lost.
    def merge_dependency(deps, k, value)
      return deps[k] = value unless deps.key?(k)
      existing = deps[k]
      ex_node = existing.is_a?(Array) ? {'required' => existing} : existing
      new_node = value.is_a?(Array) ? {'required' => value} : value
      deps[k] = {'allOf' => [ex_node, new_node]}
    end
  end
end
