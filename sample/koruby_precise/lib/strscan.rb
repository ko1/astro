# frozen_string_literal: true
#
# strscan.rb — a pure-Ruby StringScanner for koruby_precise (koruby ships no C
# strscan extension).  Covers the common surface: scan / skip / check / match? /
# scan_until / skip_until / getch / peek / pos / eos? / rest / terminate / reset
# and capture access via [].  Matching is anchored at the current position
# (StringScanner semantics), realised through Regexp#match on the remaining
# substring with an empty pre_match.
class StringScanner
  Version = "3.1.6"
  Id = "$Id$"
  class Error < StandardError; end

  attr_reader :string

  # CRuby's scan pointer counts BYTES, so @pos is a byte offset.  Matching needs
  # a character offset, so @cpos mirrors it and is recomputed lazily (nil) after
  # an operation that can land mid-character.
  def initialize(string, _dup = false, fixed_anchor: false)
    @string = string.to_str
    @pos = 0
    @cpos = 0
    @prev = 0
    @md = nil
    @fixed_anchor = fixed_anchor ? true : false
  end

  def self.must_C_version; self; end

  def fixed_anchor? = @fixed_anchor

  def string=(s)
    @string = s.to_str
    @pos = 0
    @cpos = 0
    @prev = 0
    @md = nil
    s
  end

  def __cpos
    @cpos ||= @string.byteslice(0, @pos).to_s.length
  end
  private :__cpos

  def pos; @pos; end
  def pos=(n)
    n = n.to_int if !n.is_a?(Integer) && n.respond_to?(:to_int)
    raise TypeError, "no implicit conversion into Integer" unless n.is_a?(Integer)
    n += @string.bytesize if n < 0
    raise RangeError, "index out of range" if n < 0 || n > @string.bytesize
    @cpos = nil
    @pos = n
  end
  alias_method :pointer, :pos
  alias_method :pointer=, :pos=

  def eos?; @pos >= @string.bytesize; end
  def rest; @string.byteslice(@pos, @string.bytesize - @pos) || ""; end
  def rest_size; @string.bytesize - @pos; end
  def reset; @pos = 0; @cpos = 0; @prev = 0; @md = nil; self; end
  def terminate; @pos = @string.bytesize; @cpos = nil; @md = nil; self; end
  def beginning_of_line?
    @pos == 0 || @string.byteslice(@pos - 1, 1) == "\n"
  end
  alias bol? beginning_of_line?

  # --- anchored match at @pos -----------------------------------------------
  # Both Regexp and String patterns are accepted (String → literal match at the
  # current position, like CRuby StringScanner).  Returns [matched, md|nil] or nil.
  # Matches via Regexp#match(string, pos) + begin(0)==pos so there is NO O(n)
  # substring copy per scan (the naive `@string[@pos..]` slice made scanning a
  # large subject O(n^2)).
  # A Regexp match on a broken string raises (a String pattern does not).  The
  # validity is cached per (bytesize, encoding): valid_encoding? walks the string.
  def __check_encoding(pattern)
    return unless pattern.is_a?(Regexp)
    sz = @string.bytesize
    enc = @string.encoding
    unless @venc_size == sz && @venc_enc == enc
      @venc_size = sz; @venc_enc = enc; @venc_ok = @string.valid_encoding?
    end
    raise ArgumentError, "invalid byte sequence in #{enc}" unless @venc_ok
  end
  private :__check_encoding

  # `\G(?:pat)`: the engine then tries only @pos.  Searching forward from @pos
  # and checking where the match began made every failing scan O(rest), so a
  # tokenizer was O(n^2) (w12 A09).  Cached by identity, bounded.
  G_CACHE = {}.compare_by_identity
  def self.__anchored(re)
    G_CACHE[re] ||= begin
      G_CACHE.clear if G_CACHE.size >= 4096
      Regexp.new("\\G(?:#{re.source}#{(re.options & Regexp::EXTENDED) != 0 ? "\n" : ""})", re.options)
    end
  end

  def try(pattern)
    __check_encoding(pattern)
    # A String pattern still produces match data in CRuby (#matched? is true and
    # #[0] is the text), so it goes through an escaped Regexp.
    pattern = __to_pattern(pattern)
    if !@fixed_anchor && __anchored?(pattern)
      # \A / ^ / $ refer to the REST of the string unless fixed_anchor is set,
      # so those patterns have to match a real slice (the offset form below
      # would anchor them to the original string).
      m = pattern.match(rest)
      @md_base = @pos                    # m was taken on the rest: #pre_match puts the head back
      (m && m.begin(0) == 0) ? [m[0], m] : nil
    else
      # BYTE-offset match: converting @pos to a character index here walked the
      # subject on every scan, which made scanning a large string O(n^2).
      m = StringScanner.__anchored(pattern).send(:__match_byte, @string, @pos)
      @md_base = 0
      m ? [m[0], m] : nil
    end
  end
  private :try

  # Anchor detection is per-Regexp and memoized: the offset form of #match
  # avoids an O(n) slice per scan, and only anchored patterns give it up.
  # \A \Z \z \G ^ $ outside a character class ("[^a]" is a negation, not an
  # anchor: taking it for one sent rexml's ATTLISTDECL_END down the slicing path).
  def self.__anchor_in?(src)
    i = 0; n = src.length; depth = 0
    while i < n
      ch = src[i]
      if ch == "\\"
        return true if depth == 0 && i + 1 < n && "AZzG".include?(src[i + 1])
        i += 2
        next
      end
      if depth > 0
        if ch == "[" then depth += 1
        elsif ch == "]" then depth -= 1
        end
      elsif ch == "["
        depth = 1
        i += 1
        i += 1 if src[i] == "^"
        i += 1 if src[i] == "]"                        # a leading ] is a member
        next
      elsif ch == "^" || ch == "$"
        return true
      end
      i += 1
    end
    false
  end

  # scan/skip/check/match? take a Regexp or a String (which still produces match
  # data in CRuby, so it goes through an escaped Regexp); anything else is a
  # TypeError.
  def __to_pattern(pattern)
    return pattern if pattern.is_a?(Regexp)
    if !pattern.is_a?(String) && pattern.respond_to?(:to_str)
      pattern = pattern.to_str
    end
    unless pattern.is_a?(String)
      raise TypeError, "wrong argument type #{pattern.class} (expected Regexp)"
    end
    (@str_re_cache ||= {})[pattern] ||= Regexp.new(Regexp.escape(pattern))
  end
  private :__to_pattern

  def __anchored?(re)
    @anchor_cache ||= {}
    c = @anchor_cache[re]
    return c unless c.nil?
    @anchor_cache[re] = StringScanner.__anchor_in?(re.source)
  end
  private :__anchored?

  def scan(pattern)
    r = try(pattern) or return (@md = nil)
    @md = r[1]
    @prev = @pos
    @pos += r[0].bytesize
    @cpos += r[0].length if @cpos
    r[0]
  end

  def skip(pattern)
    s = scan(pattern)
    s && s.bytesize
  end

  def check(pattern)
    r = try(pattern)
    @md = r && r[1]
    r && r[0]
  end

  def match?(pattern)
    r = try(pattern)
    @md = r && r[1]
    r && r[0].bytesize
  end

  def scan_until(re)
    __check_encoding(re)
    if !@fixed_anchor && re.is_a?(Regexp) && __anchored?(re)
      sub = rest
      m = re.match(sub) or return (@md = nil)
      @md = m
      @md_base = @pos
      stop_b = @pos + m.byteoffset(0)[1]
      ncp = nil
    else
      m = __to_pattern(re).send(:__match_byte, @string, @pos) or return (@md = nil)
      @md = m
      @md_base = 0
      stop_b = m.byteoffset(0)[1]        # end of the match, absolute bytes
      ncp = nil                          # character cursor is recomputed on demand
    end
    s = @string.byteslice(@pos, stop_b - @pos)
    @prev = @pos
    @pos = stop_b
    @cpos = ncp
    s
  end

  def skip_until(re)
    s = scan_until(re)
    s && s.bytesize
  end

  def getch
    return nil if eos?
    # Slice by BYTES: @string[__cpos] walked the subject from the start on every
    # call, which made scanning a large multibyte string O(n^2).  No character is
    # wider than 4 bytes in any encoding koruby knows, so the first character of
    # a 4-byte window is always whole.
    c = @string.byteslice(@pos, 4)[0]
    @prev = @pos
    @pos += c.bytesize
    @cpos += 1 if @cpos
    @md = c
    c
  end

  # #peek counts BYTES (CRuby), so a multibyte character can be cut in half.
  def peek(n)
    n = n.to_int if !n.is_a?(Integer) && n.respond_to?(:to_int)
    raise TypeError, "no implicit conversion into Integer" unless n.is_a?(Integer)
    raise ArgumentError, "negative string size (or size too big)" if n < 0
    @string.byteslice(@pos, n) || ""
  end
  alias peep peek

  # #getch / #get_byte also set the last-match state, but with no capture groups;
  # those store the read text directly in @md instead of a MatchData.
  def [](i)
    return nil unless @md
    unless i.is_a?(Integer) || i.is_a?(String) || i.is_a?(Symbol)
      raise TypeError, "no implicit conversion of #{i.class} into Integer" unless i.respond_to?(:to_int)
      i = i.to_int
    end
    if @md.is_a?(String)
      raise IndexError, "undefined group name reference: #{i}" unless i.is_a?(Integer)
      return i == 0 || i == -1 ? @md : nil
    end
    @md[i]
  end

  def matched;    @md && (@md.is_a?(String) ? @md : @md[0]); end
  def matched?;   !@md.nil?; end
  def pre_match
    return nil unless @md
    return @string.byteslice(0, @pos - @md.bytesize) if @md.is_a?(String)
    @md_base.to_i > 0 ? @string.byteslice(0, @md_base) + @md.pre_match : @md.pre_match
  end
  def post_match; @md && (@md.is_a?(String) ? rest : @md.post_match); end

  def matched_size; @md && (@md.is_a?(String) ? @md.bytesize : @md[0].bytesize); end
  def size;         @md && (@md.is_a?(String) ? 1 : @md.size); end
  def captures;     @md && (@md.is_a?(String) ? [] : @md.captures); end
  def named_captures; @md && !@md.is_a?(String) ? @md.named_captures : {}; end

  # values_at(*idx) — the given capture groups (nil when nothing matched).
  def values_at(*idx)
    return nil unless @md
    idx.map { |i| self[i] }
  end

  # check_until — scan_until without moving the pointer.
  def check_until(re)
    save = @pos
    r = scan_until(re)
    @pos = save
    r
  end

  # peek_byte / scan_byte — the byte at the pointer as an Integer.
  def peek_byte
    b = @string.byteslice(@pos, 1)
    b && !b.empty? ? b.getbyte(0) : nil
  end

  def scan_byte
    b = get_byte
    b && !b.empty? ? b.getbyte(0) : nil
  end

  # scan_integer(base: 10) — an optionally-signed integer literal in `base`.
  def scan_integer(base: 10)
    unless @string.encoding.ascii_compatible?
      raise Encoding::CompatibilityError,
            "ASCII incompatible encoding: #{@string.encoding.name}"
    end
    digits = case base
             when 10 then "[0-9]"
             when 16 then "(?:0[xX])?[0-9a-fA-F]"
             when 8  then "(?:0[oO])?[0-7]"
             when 2  then "(?:0[bB])?[01]"
             else raise ArgumentError, "Unsupported integer base: #{base}, expected 10 or 16"
             end
    s = scan(/[+-]?#{digits}+/) or return nil
    Integer(s, base)
  end

  # get_byte — one BYTE (unlike #getch, which is a character).
  def get_byte
    b = @string.byteslice(@pos, 1)
    return nil if b.nil? || b.empty?
    @prev = @pos
    @pos += 1
    @cpos = nil                                       # may land mid-character
    @md = b
    b
  end
  alias getbyte get_byte

  # scan_full(pattern, advance_pointer_p, return_string_p)
  def scan_full(pattern, advance_pointer_p = true, return_string_p = true)
    save = @pos
    savec = @cpos
    r = scan(pattern)
    (@pos = save; @cpos = savec) unless advance_pointer_p
    return nil if r.nil?
    return_string_p ? r : r.bytesize
  end

  # search_full(pattern, advance_pointer_p, return_string_p)
  def search_full(pattern, advance_pointer_p = true, return_string_p = true)
    save = @pos
    savec = @cpos
    r = scan_until(pattern)
    (@pos = save; @cpos = savec) unless advance_pointer_p
    return nil if r.nil?
    return_string_p ? r : r.bytesize
  end

  # << / concat — append to the scanned string (position untouched).
  def concat(str)
    raise TypeError, "no implicit conversion of #{str.class} into String" unless str.is_a?(String)
    @string = @string + str
    self
  end
  alias << concat

  # unscan — undo the last match (CRuby raises when there is none).
  def unscan
    raise StringScanner::Error, "unscan failed: previous match record not exist" unless @md
    @pos = @prev
    @cpos = nil
    @md = nil
    self
  end

  def exist?(pattern)
    save = @pos
    savec = @cpos
    r = skip_until(pattern)
    @pos = save
    @cpos = savec
    r
  end

  def rest?; !eos?; end
  def charpos; __cpos; end

  # CRuby shows at most 5 bytes on each side of the pointer, String#dump-ed.
  def inspect
    return "#<StringScanner fin>" if eos?
    len = @string.bytesize
    tail = len - @pos > 5 ? @string.byteslice(@pos, 5) + "..." : rest
    return "#<StringScanner #{@pos}/#{len} @ #{tail.dump}>" if @pos == 0
    head = @pos > 5 ? "..." + @string.byteslice(@pos - 5, 5) : @string.byteslice(0, @pos)
    "#<StringScanner #{@pos}/#{len} #{head.dump} @ #{tail.dump}>"
  end
end

ScanError = StringScanner::Error unless defined?(ScanError)
