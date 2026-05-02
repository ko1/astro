# frozen_string_literal: true
#
# astrogre — Ruby-friendly layer over astrogre_ext.so.
#
# The C extension exposes Pattern with low-level position-returning
# methods (`_match_offsets` / `_captures_offsets` / etc.).  This file
# adds the Ruby-Regexp-compatible public surface — `match` returns an
# ASTrogre::MatchData, `=~` returns a position, `===` is case-equality,
# and so on.  The intent: drop-in feel for code that's used to Ruby's
# Regexp + MatchData.
#
#   require "astrogre"
#   re = ASTrogre.compile(/(?<host>\w+):(?<port>\d+)/)
#   md = re.match("ex:8080")
#   md[:host]            # => "ex"
#   re =~ "x ex:8080 y"  # => 2
#   re === "ex:80"       # => true

require_relative "astrogre_ext"

module ASTrogre
  # Mimic of Ruby's `MatchData`.  Positions stored internally in BYTE
  # space (that's what the engine returns); `begin`/`end`/`offset`
  # convert to character offsets on demand to match Ruby semantics on
  # multibyte-encoded input.  ASCII-8BIT inputs short-circuit the
  # conversion (byte == char).
  class MatchData
    # @param string  [String] the input that produced the match
    # @param positions [Array<[Integer,Integer]|nil>] byte spans, [0] = whole
    # @param names    [Hash{String=>Integer}] named-capture → group index
    # @param pattern  [ASTrogre::Pattern] the originating pattern
    def initialize(string, positions, names, pattern = nil)
      @string    = string
      @positions = positions
      @names     = names
      @pattern   = pattern
      @bytewise  = (string.encoding == Encoding::ASCII_8BIT) ||
                   (string.encoding == Encoding::US_ASCII)
    end

    # Substring access by index, name, or range.  Mirrors MatchData#[].
    def [](idx, len = nil)
      return to_a[idx, len] if len
      case idx
      when Range
        to_a[idx]
      else
        i = resolve_index(idx)
        return nil unless i
        span = @positions[i]
        span && @string.byteslice(span[0], span[1] - span[0])
      end
    end

    # Character index of the start of capture #idx, like Ruby
    # MatchData#begin.
    def begin(idx)
      i = resolve_index(idx)
      i && (s = @positions[i]) && byte_to_char(s[0])
    end

    # Character index of the end of capture #idx.
    def end(idx)
      i = resolve_index(idx)
      i && (s = @positions[i]) && byte_to_char(s[1])
    end

    # `[char_begin, char_end]` pair; nil-pair when capture didn't fire.
    def offset(idx)
      i = resolve_index(idx)
      return [nil, nil] unless i && @positions[i]
      [byte_to_char(@positions[i][0]), byte_to_char(@positions[i][1])]
    end

    # `[byte_begin, byte_end]` pair; bypasses encoding conversion.
    # Matches Ruby's MatchData#byteoffset (3.2+).
    def byteoffset(idx)
      i = resolve_index(idx)
      return [nil, nil] unless i && @positions[i]
      @positions[i].dup
    end

    def to_a
      @positions.map { |se| se && @string.byteslice(se[0], se[1] - se[0]) }
    end
    alias deconstruct to_a

    def captures
      to_a.drop(1)
    end

    def named_captures
      @names.each_with_object({}) do |(name, idx), h|
        span = @positions[idx]
        h[name] = span && @string.byteslice(span[0], span[1] - span[0])
      end
    end

    def deconstruct_keys(keys)
      h = {}
      @names.each do |name, idx|
        sym = name.to_sym
        next if keys && !keys.include?(sym)
        span = @positions[idx]
        h[sym] = span && @string.byteslice(span[0], span[1] - span[0])
      end
      h
    end

    def values_at(*indices)
      indices.map { |i| self[i] }
    end

    # Names of `(?<…>)` groups in declaration order — mirrors
    # MatchData#names / Regexp#names.
    def names
      @names.keys
    end

    def size
      @positions.size
    end
    alias length size

    def string
      @string
    end

    def regexp
      @pattern
    end

    def pre_match
      @positions[0] ? @string.byteslice(0, @positions[0][0]) : ""
    end

    def post_match
      span = @positions[0]
      span ? @string.byteslice(span[1], @string.bytesize - span[1]) : ""
    end

    def to_s
      self[0]
    end

    def ==(other)
      other.is_a?(MatchData) &&
        @string    == other.instance_variable_get(:@string) &&
        @positions == other.instance_variable_get(:@positions) &&
        @names     == other.instance_variable_get(:@names)
    end
    alias eql? ==

    def hash
      [@string, @positions, @names].hash
    end

    def inspect
      groups = (1...@positions.size).map { |i| "#{i}:#{self[i].inspect}" }
      groups.empty? \
        ? "#<ASTrogre::MatchData #{self[0].inspect}>"
        : "#<ASTrogre::MatchData #{self[0].inspect} #{groups.join(' ')}>"
    end

    private

    def resolve_index(idx)
      case idx
      when Integer
        i = idx < 0 ? @positions.size + idx : idx
        i >= 0 && i < @positions.size ? i : nil
      when Symbol, String
        @names[idx.to_s]
      end
    end

    # Convert a byte offset into a character offset under the input
    # string's encoding.  Ruby's MatchData does the same thing on every
    # `begin`/`end` call — O(n) in the worst case but trivially short-
    # circuited for ASCII-8BIT / US-ASCII strings where byte == char.
    def byte_to_char(byte_off)
      return byte_off if @bytewise || byte_off == 0
      @string.byteslice(0, byte_off).length
    end
  end

  # ASTrogre::Pattern is the C-level low-level class.  Here we add the
  # public Regexp-compatible surface on top so `Pattern#match` returns a
  # MatchData, `=~` and `===` work as expected, etc.  The C-defined
  # `_match_offsets` / `_captures_offsets` etc. remain available for
  # callers (mainly tests) that want raw byte positions without paying
  # for the MatchData wrapper.
  class Pattern
    # Ruby Regexp::IGNORECASE / MULTILINE / EXTENDED — re-export so
    # callers can build masks without `require "regexp"`.
    IGNORECASE = ::Regexp::IGNORECASE
    MULTILINE  = ::Regexp::MULTILINE
    EXTENDED   = ::Regexp::EXTENDED

    # Save the C-level methods we're about to override / re-shape.
    alias _match_p_at_zero match?         # original arity-1 match?
    alias _named_groups_h  named_groups   # raw {name => idx} introspection
    private :_match_p_at_zero, :_named_groups_h

    # Run the matcher and return ASTrogre::MatchData (or nil).  Optional
    # `pos` (byte offset) resumes the scan from there — same role as
    # Ruby's Regexp#match second argument.
    def match(input, pos = 0)
      positions = pos.zero? ? _match_offsets(input)
                            : _match_at_offsets(input, pos)
      positions && MatchData.new(input, positions, _named_groups_h, self)
    end

    # Bool-only match check, optional resume offset.  At pos==0 we use
    # the no-allocation C path; otherwise we fall through to the
    # position-aware variant (which does allocate the offsets array,
    # but still skips MatchData wrapping).
    def match?(input, pos = 0)
      pos.zero? ? _match_p_at_zero(input)
                : !_match_at_offsets(input, pos).nil?
    end

    # `=~` operator.  Returns the *character* offset of the first match,
    # nil on miss.  For multibyte-encoded inputs the byte offset the
    # engine produces is converted via the same prefix-length trick that
    # MatchData#begin uses.
    def =~(input)
      positions = _match_offsets(input)
      return nil unless positions
      byte_off = positions[0][0]
      enc = input.encoding
      return byte_off if enc == Encoding::ASCII_8BIT || enc == Encoding::US_ASCII
      return byte_off if byte_off == 0
      input.byteslice(0, byte_off).length
    end

    # Case equality — the operator behind `case … when re`.  Mirrors
    # Regexp#===.  Accepts only String (matching Ruby's behaviour;
    # `re === some_array` is always false).
    def ===(input)
      input.is_a?(String) && match?(input)
    end

    # Substring captures (sans whole match), like MatchData#captures —
    # convenience wrapper around `match(input)&.captures`.
    def captures(input)
      m = match(input)
      m && m.captures
    end

    # Two arities, mirroring Ruby:
    #   re.named_captures              # => {"name" => [idx, …]}  (introspection)
    #   re.named_captures(input)       # => {"name" => "substring"}|nil  (after match)
    # The introspection form returns Ruby-Regexp-shaped arrays of
    # indices so callers can swap astrogre in for `re.named_captures`
    # without code changes.
    def named_captures(input = nil)
      if input.nil?
        h = {}
        _named_groups_h.each { |name, idx| (h[name] ||= []) << idx }
        h
      else
        m = match(input)
        m && m.named_captures
      end
    end

    # Backwards compat: the original name we shipped before aligning
    # with Ruby.  Returns the flat `{name => idx}` form (single index
    # per name).  Prefer `named_captures` (no arg) for new code.
    def named_groups
      _named_groups_h
    end

    # Enumerate every match as MatchData (block form too).  Mirrors
    # String#scan's positional behaviour but returns MatchData objects
    # rather than capture arrays.  Resumes via `_match_at_offsets` so
    # anchors like `^` and `\A` see the original surrounding bytes
    # rather than a slice that pretends to start fresh.
    def match_all(input)
      result = block_given? ? nil : []
      names  = named_groups
      pos    = 0
      total  = input.bytesize
      while pos <= total
        positions = _match_at_offsets(input, pos)
        break unless positions
        m = MatchData.new(input, positions, names, self)
        block_given? ? yield(m) : result << m
        s, e = positions[0]
        pos = (e == s) ? e + 1 : e
      end
      result
    end

    # Pattern source (between the `/`s).
    # `source` is C-defined — kept as the canonical name.

    # Flag predicates.  /x is parse-time-only so it's never set on the
    # compiled pattern — `extended?` always returns false here.
    def casefold?  = (options & IGNORECASE) != 0
    def multiline? = (options & MULTILINE)  != 0
    def extended?  = (options & EXTENDED)   != 0

    def names
      named_groups.keys
    end

    def to_s
      flags = ""
      flags << "i" if casefold?
      flags << "m" if multiline?
      flags << "x" if extended?
      negs = ""
      negs << "i" unless casefold?
      negs << "m" unless multiline?
      negs << "x" unless extended?
      "(?#{flags}-#{negs}:#{source})"
    end

    def inspect
      flags = ""
      flags << "i" if casefold?
      flags << "m" if multiline?
      flags << "x" if extended?
      "/#{source}/#{flags}"
    end
  end
end
