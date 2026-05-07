require 'json'
require 'rbconfig'
require_relative '../arjsv'  # arjsv.so (built by extconf)
require_relative 'arjsv/version'

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
  def self.schema(schema_obj)
    builder = Builder.new
    builder.preregister_defs(schema_obj)
    body = builder.lower(schema_obj)
    root = _alloc_validate_root(body)
    builder.entries.unshift(root)
    Schema._new(root, builder.consts, builder.entries)
  end

  class Schema
    # Public wrapper around the C-level _compile that supplies the right
    # Ruby header cflags by default.
    def compile!(extra_cflags = nil)
      cflags = extra_cflags ? "#{RUBY_HEADER_CFLAGS} #{extra_cflags}" : RUBY_HEADER_CFLAGS
      _compile(cflags)
    end
  end

  # Walks a schema hash and emits a NODE tree via the C-level _alloc_*
  # functions.  Constants used by enum / const land in @consts and are
  # referenced by index from the relevant NODEs.
  class Builder
    attr_reader :consts, :entries

    SUPPORTED_KEYWORDS = %w[
      type properties required items additionalItems
      additionalProperties patternProperties propertyNames
      minimum maximum exclusiveMinimum exclusiveMaximum multipleOf
      minLength maxLength pattern format
      minItems maxItems uniqueItems
      minProperties maxProperties
      const enum
      allOf anyOf oneOf not
      if then else
      $ref $schema $id $comment $defs definitions
      title description default examples
    ].freeze

    def initialize
      @consts = []
      @entries = []           # secondary AST roots (e.g. $defs targets)
      @defs_idx = {}          # "<name>" => @consts slot holding the target NODE wrapper
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
        # draft-07 semantics: if `$ref` is present, sibling keywords are
        # ignored.  json_schemer matches this for draft-07.
        return lower_ref(schema['$ref']) if schema.key?('$ref')
        warn_unknown_keywords(schema)

        nodes = []
        nodes << lower_type(schema['type'])           if schema.key?('type')
        nodes << lower_required(schema['required'])   if schema.key?('required')
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
      keys.reverse.inject(Arjsv._alloc_pass) do |tail, key|
        key_str = key.to_s
        Arjsv._alloc_required(key_str, intern_key(key_str), tail)
      end
    end

    def lower_properties(props)
      raise ArgumentError, "properties must be Hash" unless props.is_a?(Hash)
      # Stable iteration: sort keys so identical schemas produce identical
      # ASTs (and thus identical SD hashes) regardless of input ordering.
      props.sort.reverse.inject(Arjsv._alloc_pass) do |tail, (key, sub)|
        key_str = key.to_s
        Arjsv._alloc_property(key_str, intern_key(key_str), lower(sub), tail)
      end
    end

    # Park a property-name as a deduplicated frozen String in @consts so the
    # runtime can do `rb_hash_lookup2(c->data, c->consts[idx])` without
    # allocating a fresh String per validation.  `-key_str` (= dedup_to_fstring)
    # gives an interned, frozen, hash-cached String — Ruby's standard
    # idiom for hash-key reuse.
    def intern_key(key_str)
      intern_const(-key_str)
    end

    def lower_items(schema)
      items = schema['items']
      case items
      when Hash, true, false
        Arjsv._alloc_items_uniform(lower(items))
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
      regex = Regexp.new(pat_str)
      Arjsv._alloc_pattern(pat_str, intern_const(regex))
    end

    # `format` is reduced to a built-in regex (or no-op for unknown formats,
    # which JSON Schema permits).  Only a small common subset is provided.
    FORMAT_REGEXES = {
      'date'      => /\A\d{4}-\d{2}-\d{2}\z/,
      'date-time' => /\A\d{4}-\d{2}-\d{2}[Tt ]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:[Zz]|[+-]\d{2}:\d{2})\z/,
      'time'      => /\A\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:[Zz]|[+-]\d{2}:\d{2})?\z/,
      'email'     => /\A[^\s@]+@[^\s@]+\.[^\s@]+\z/,
      'uri'       => /\A[a-zA-Z][a-zA-Z0-9+.\-]*:\S*\z/,
      'ipv4'      => /\A(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\z/,
      'ipv6'      => /\A[0-9a-fA-F:]+\z/,  # loose
      'uuid'      => /\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\z/,
    }.freeze

    def lower_format(fmt)
      regex = FORMAT_REGEXES[fmt]
      return Arjsv._alloc_pass if regex.nil?  # unknown format = annotation only
      Arjsv._alloc_pattern("__format_#{fmt}__", intern_const(regex))
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

    # ---- $ref --------------------------------------------------------

    def lower_ref(ref_str)
      raise ArgumentError, "$ref must be String" unless ref_str.is_a?(String)
      m = ref_str.match(%r{\A\#/(?:\$defs|definitions)/([^/]+)\z})
      raise NotImplementedError, "only #/$defs/<name> $refs supported, got #{ref_str.inspect}" unless m
      idx = @defs_idx[m[1]]
      raise ArgumentError, "unknown $ref target: #{ref_str}" if idx.nil?
      Arjsv._alloc_ref(ref_str, idx)
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
      raise ArgumentError, "enum must be non-empty" if values.empty?
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
  end
end
