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

  def initialize(string, _dup = false, fixed_anchor: false)
    @string = string.to_str
    @pos = 0
    @md = nil
    @fixed_anchor = fixed_anchor ? true : false
  end

  def fixed_anchor? = @fixed_anchor

  def string=(s)
    @string = s.to_str
    @pos = 0
    @md = nil
    s
  end

  def pos; @pos; end
  def pos=(n)
    n += @string.length if n < 0
    raise RangeError, "index out of range" if n < 0 || n > @string.length
    @pos = n
  end
  alias charpos pos
  alias pointer pos

  def eos?; @pos >= @string.length; end
  def rest; @string[@pos..] || ""; end
  def rest_size; @string.length - @pos; end
  def reset; @pos = 0; @md = nil; self; end
  def terminate; @pos = @string.length; @md = nil; self; end
  def beginning_of_line?; @pos == 0 || @string[@pos - 1] == "\n"; end
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
    pattern = (@str_re_cache ||= {})[pattern] ||= Regexp.new(Regexp.escape(pattern)) if pattern.is_a?(String)
    if !@fixed_anchor && __anchored?(pattern)
      # \A / ^ / $ refer to the REST of the string unless fixed_anchor is set,
      # so those patterns have to match a real slice (the offset form below
      # would anchor them to the original string).
      m = pattern.match(@string[@pos..] || "")
      (m && m.begin(0) == 0) ? [m[0], m] : nil
    else
      m = pattern.match(@string, @pos)
      (m && m.begin(0) == @pos) ? [m[0], m] : nil
    end
  end
  private :try

  # Anchor detection is per-Regexp and memoized: the offset form of #match
  # avoids an O(n) slice per scan, and only anchored patterns give it up.
  ANCHOR_RE__ = /(?<!\\)(?:\\[AZzG]|[\^$])/
  private_constant :ANCHOR_RE__

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
    @pos += r[0].length
    r[0]
  end

  def skip(pattern)
    s = scan(pattern)
    s && s.length
  end

  def check(pattern)
    r = try(pattern)
    @md = r && r[1]
    r && r[0]
  end

  def match?(pattern)
    r = try(pattern)
    @md = r && r[1]
    r && r[0].length
  end

  def scan_until(re)
    if !@fixed_anchor && re.is_a?(Regexp) && __anchored?(re)
      m = re.match(@string[@pos..] || "") or return (@md = nil)
      @md = m
      stop = @pos + m.begin(0) + m[0].length
    else
      m = (re.is_a?(String) ? Regexp.new(Regexp.escape(re)) : re).match(@string, @pos) or
        return (@md = nil)
      @md = m
      stop = m.begin(0) + m[0].length    # end of the match, absolute
    end
    s = @string[@pos...stop]
    @pos = stop
    s
  end

  def skip_until(re)
    s = scan_until(re)
    s && s.length
  end

  def getch
    return nil if eos?
    c = @string[@pos]
    @pos += 1
    @md = c
    c
  end

  # #peek counts BYTES (CRuby), so a multibyte character can be cut in half.
  def peek(n)
    n = n.to_int if !n.is_a?(Integer) && n.respond_to?(:to_int)
    raise TypeError, "no implicit conversion into Integer" unless n.is_a?(Integer)
    raise ArgumentError, "negative string size (or size too big)" if n < 0
    @string.byteslice(__byte_pos, n) || ""
  end
  alias peep peek

  # The scan pointer as a byte offset (#pos is characters here).
  def __byte_pos = @string[0, @pos].to_s.bytesize
  private :__byte_pos

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
  def pre_match;  @md && (@md.is_a?(String) ? @string[0, @pos - @md.length] : @md.pre_match); end
  def post_match; @md && (@md.is_a?(String) ? @string[@pos..] : @md.post_match); end

  def matched_size; @md && (@md.is_a?(String) ? @md.length : @md[0].length); end
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
    b = @string.byteslice(__byte_pos, 1)
    b && !b.empty? ? b.getbyte(0) : nil
  end

  def scan_byte
    b = get_byte
    b && !b.empty? ? b.getbyte(0) : nil
  end

  # scan_integer(base: 10) — an optionally-signed integer literal in `base`.
  def scan_integer(base: 10)
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
    bp = __byte_pos
    b = @string.byteslice(bp, 1)
    return nil if b.nil? || b.empty?
    @pos = @string.byteslice(0, bp + 1).to_s.length   # may land mid-character
    @md = b
    b
  end
  alias getbyte get_byte

  # scan_full(pattern, advance_pointer_p, return_string_p)
  def scan_full(pattern, advance_pointer_p = true, return_string_p = true)
    save = @pos
    r = scan(pattern)
    @pos = save unless advance_pointer_p
    return nil if r.nil?
    return_string_p ? r : (r.length + (advance_pointer_p ? 0 : 0))
  end

  # search_full(pattern, advance_pointer_p, return_string_p)
  def search_full(pattern, advance_pointer_p = true, return_string_p = true)
    save = @pos
    r = scan_until(pattern)
    @pos = save unless advance_pointer_p
    return nil if r.nil?
    return_string_p ? r : r.length
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
    @pos = @md.begin(0) if @md.respond_to?(:begin)
    @md = nil
    self
  end

  def exist?(pattern)
    save = @pos
    r = skip_until(pattern)
    @pos = save
    r
  end

  def rest?; !eos?; end
  def charpos; @string[0, @pos].length; end
  def fixed_anchor?; false; end

  def inspect
    "#<StringScanner #{@pos}/#{@string.length}>"
  end
end

ScanError = StringScanner::Error unless defined?(ScanError)
