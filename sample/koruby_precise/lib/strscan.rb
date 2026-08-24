# frozen_string_literal: true
#
# strscan.rb — a pure-Ruby StringScanner for koruby_precise (koruby ships no C
# strscan extension).  Covers the common surface: scan / skip / check / match? /
# scan_until / skip_until / getch / peek / pos / eos? / rest / terminate / reset
# and capture access via [].  Matching is anchored at the current position
# (StringScanner semantics), realised through Regexp#match on the remaining
# substring with an empty pre_match.
class StringScanner
  attr_reader :string

  def initialize(string, _dup = false)
    @string = string.to_str
    @pos = 0
    @md = nil
  end

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

  # --- anchored match at @pos -----------------------------------------------
  # Both Regexp and String patterns are accepted (String → literal match at the
  # current position, like CRuby StringScanner).  Returns [matched, md|nil] or nil.
  # Matches via Regexp#match(string, pos) + begin(0)==pos so there is NO O(n)
  # substring copy per scan (the naive `@string[@pos..]` slice made scanning a
  # large subject O(n^2)).
  def try(pattern)
    if pattern.is_a?(String)
      @string[@pos, pattern.length] == pattern ? [pattern, nil] : nil
    else
      m = pattern.match(@string, @pos)
      (m && m.begin(0) == @pos) ? [m[0], m] : nil
    end
  end
  private :try

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
    m = re.match(@string, @pos) or return (@md = nil)
    @md = m
    stop = m.begin(0) + m[0].length      # end of the match, absolute
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
    @md = nil
    c
  end

  def peek(n); @string[@pos, n] || ""; end
  alias peep peek

  def [](i)
    @md && @md[i]
  end

  def matched;    @md && @md[0]; end
  def matched?;   !@md.nil?; end
  def pre_match;  @md && @md.pre_match; end
  def post_match; @md && @md.post_match; end

  def matched_size; @md && @md[0].length; end
  def size;         @md && @md.size; end
  def captures;     @md && @md.captures; end
  def named_captures; @md ? @md.named_captures : {}; end

  # values_at(*idx) — the given capture groups (nil when nothing matched).
  def values_at(*idx)
    return nil unless @md
    idx.map { |i| @md[i] }
  end

  # get_byte — one BYTE (unlike #getch, which is a character).
  def get_byte
    return nil if eos?
    b = @string.byteslice(@pos, 1)
    @pos += 1
    @md = nil
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
    raise ScanError, "unscan failed: previous match record not exist" unless @md
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

class ScanError < StandardError; end unless defined?(ScanError)
