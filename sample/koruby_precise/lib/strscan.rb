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
  def try(pattern)
    # A String pattern still produces match data in CRuby (#matched? is true and
    # #[0] is the text), so it goes through an escaped Regexp.
    pattern = __to_pattern(pattern)
    if !@fixed_anchor && __anchored?(pattern)
      # \A / ^ / $ refer to the REST of the string unless fixed_anchor is set,
      # so those patterns have to match a real slice (the offset form below
      # would anchor them to the original string).
      m = pattern.match(rest)
      (m && m.begin(0) == 0) ? [m[0], m] : nil
    else
      cp = __cpos
      m = pattern.match(@string, cp)
      (m && m.begin(0) == cp) ? [m[0], m] : nil
    end
  end
  private :try

  # Anchor detection is per-Regexp and memoized: the offset form of #match
  # avoids an O(n) slice per scan, and only anchored patterns give it up.
  ANCHOR_RE__ = /(?<!\\)(?:\\[AZzG]|[\^$])/
  private_constant :ANCHOR_RE__

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
    @anchor_cache[re] = ANCHOR_RE__.match?(re.source)
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
    if !@fixed_anchor && re.is_a?(Regexp) && __anchored?(re)
      sub = rest
      m = re.match(sub) or return (@md = nil)
      @md = m
      stop_b = @pos + m.byteoffset(0)[1]
      ncp = nil
    else
      cp = __cpos
      m = __to_pattern(re).match(@string, cp) or return (@md = nil)
      @md = m
      stop_b = m.byteoffset(0)[1]        # end of the match, absolute bytes
      ncp = m.end(0)
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
    c = @string[__cpos]
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
    @md && (@md.is_a?(String) ? @string.byteslice(0, @pos - @md.bytesize) : @md.pre_match)
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
