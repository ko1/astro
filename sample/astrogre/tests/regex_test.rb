#!/usr/bin/env ruby
# Comprehensive regex-engine tests for astrogre via the Ruby C ext.
#
# Ruby's own Regexp / String API are the ground truth: for each test
# we ask Ruby what it expects, then ask the same of ASTrogre and
# assert they agree (in BYTE space — see assert_same_match for why).
#
# Usage:
#   ruby tests/regex_test.rb                # interp mode (fast)
#   ruby tests/regex_test.rb --aot          # native-compile every pattern
#   ruby tests/regex_test.rb --verbose      # log every passing case
#
# Tested API surface (rubyext/astrogre_ext.c):
#   ASTrogre.compile(re_or_str, flags=0) → Pattern   (interp/cached)
#   ASTrogre.native_compile(re_or_str)    → Pattern   (AOT-baked)
#   Pattern#match?(str)        → bool
#   Pattern#match(str)         → [[s,e], [g1s,g1e], …] or nil
#   Pattern#match_all(str)     → array of group-0 [s,e] pairs
#   Pattern#count(str)         → int
#   Pattern#dump               → S-expression of AST (diagnostic)
#   Pattern#aot_compile!       → self (specialise + dlopen)

$LOAD_PATH.unshift File.expand_path("../rubyext", __dir__)
require "astrogre"

VERBOSE  = ARGV.include?("--verbose")
AOT_MODE = ARGV.include?("--aot") || ENV["ASTROGRE_AOT"] == "1"

puts "mode: #{AOT_MODE ? "aot-compiled" : "interp"}"

# Cache compiled patterns by (source, options) so AOT mode doesn't pay
# the make-build cost per assertion when the same pattern reappears.
@compile_cache = {}
def compile_pat(pat)
  key = pat.is_a?(Regexp) ? [pat.source, pat.options] : [pat, nil]
  @compile_cache[key] ||= AOT_MODE ? ASTrogre.native_compile(pat) : ASTrogre.compile(pat)
end

@pass = 0
@fail = 0
@categories = Hash.new { |h, k| h[k] = { pass: 0, fail: 0 } }
@current_category = "uncategorized"

def category(name)
  @current_category = name
  yield
end

def record_pass(tag)
  @pass += 1
  @categories[@current_category][:pass] += 1
  puts "  ok    [#{@current_category}] #{tag}" if VERBOSE
end

def record_fail(tag, msg)
  @fail += 1
  @categories[@current_category][:fail] += 1
  puts "  FAIL  [#{@current_category}] #{tag}: #{msg}"
end

# ---- Helpers ---------------------------------------------------------

# Byte-space match comparison.  Ruby's MatchData#begin returns char
# offsets when the input is UTF-8, but astrogre operates on bytes — same
# as the grep CLI does.  Force ASCII-8BIT on both pattern and input so
# Ruby returns byte offsets, then compare apples-to-apples.
def byte_regexp(pat)
  src   = pat.is_a?(Regexp) ? pat.source.b : pat.b
  flags = pat.is_a?(Regexp) ? pat.options : 0
  Regexp.new(src, flags | Regexp::NOENCODING)  # /n forces byte-space matching
end

def assert_same_match(pat, input, label: nil)
  byte_input = input.b
  byte_re    = byte_regexp(pat)

  ruby_md = byte_re.match(byte_input)
  # Pass byte-encoded input so MatchData#begin/end return byte offsets
  # (matches Ruby's behaviour with /n input on a binary string), apples
  # to apples with our engine's byte-space offsets.
  ag_md   = compile_pat(pat).match(byte_input)

  expected = ruby_md ? [ruby_md.begin(0), ruby_md.end(0)] : nil
  got      = ag_md   ? [ag_md.begin(0),   ag_md.end(0)]   : nil

  tag = label || "#{pat.inspect} / #{input.inspect}"
  if expected == got
    record_pass(tag)
  else
    record_fail(tag, "ruby=#{expected.inspect} astrogre=#{got.inspect}")
  end
end

def assert_match(pat, input, label: nil)
  ag = compile_pat(pat)
  if ag.match?(input)
    record_pass(label || "match? #{pat.inspect} / #{input.inspect}")
  else
    record_fail(label || "match? #{pat.inspect} / #{input.inspect}", "expected match")
  end
end

def assert_no_match(pat, input, label: nil)
  ag = compile_pat(pat)
  if ag.match?(input)
    record_fail(label || "no_match #{pat.inspect} / #{input.inspect}", "expected no match")
  else
    record_pass(label || "no_match #{pat.inspect} / #{input.inspect}")
  end
end

def assert_same_count(pat, input)
  ruby_n = input.b.scan(byte_regexp(pat)).size
  ag_n   = compile_pat(pat).count(input)
  if ruby_n == ag_n
    record_pass("count #{pat.inspect} on #{input[0,30].inspect}")
  else
    record_fail("count #{pat.inspect}", "ruby=#{ruby_n} astrogre=#{ag_n}")
  end
end

def assert_same_match_all(pat, input)
  ruby_re    = byte_regexp(pat)
  byte_input = input.b
  ag = compile_pat(pat)

  ruby_spans = []
  pos = 0
  while (md = ruby_re.match(byte_input, pos))
    ruby_spans << [md.begin(0), md.end(0)]
    pos = md.end(0) == md.begin(0) ? md.end(0) + 1 : md.end(0)
  end
  # match_all now returns Array<MatchData> — pull byte offsets out of
  # each (input is binary so begin == byte_begin).
  ag_spans = ag.match_all(byte_input).map { |m| [m.begin(0), m.end(0)] }

  if ruby_spans == ag_spans
    record_pass("match_all #{pat.inspect}")
  else
    record_fail("match_all #{pat.inspect}", "ruby=#{ruby_spans.inspect} astrogre=#{ag_spans.inspect}")
  end
end

def assert_same_captures(pat, input)
  ruby_re    = byte_regexp(pat)
  byte_input = input.b
  md     = ruby_re.match(byte_input)
  ag_md  = compile_pat(pat).match(byte_input)

  if md.nil?
    if ag_md.nil?
      record_pass("captures #{pat.inspect} / #{input.inspect} (no match)")
    else
      record_fail("captures #{pat.inspect}", "ruby=nil astrogre=#{ag_md.inspect}")
    end
    return
  end

  ruby_caps = (0..md.size - 1).map { |i| md[i] ? [md.begin(i), md.end(i)] : nil }
  ag_caps   = (0..ag_md.size - 1).map { |i| ag_md.begin(i) ? [ag_md.begin(i), ag_md.end(i)] : nil }
  if ruby_caps == ag_caps
    record_pass("captures #{pat.inspect} / #{input.inspect}")
  else
    record_fail("captures #{pat.inspect}", "ruby=#{ruby_caps.inspect} astrogre=#{ag_caps.inspect}")
  end
end

def assert_dump_contains(pat, substr)
  d = compile_pat(pat).dump
  if d.include?(substr)
    record_pass("dump #{pat.inspect} contains #{substr.inspect}")
  else
    record_fail("dump #{pat.inspect}", "expected #{substr.inspect} in #{d.strip.inspect}")
  end
end

# Sweep helper — `pat, [matching, non_matching]` arrays.
def sweep(pat, yes_inputs, no_inputs)
  yes_inputs.each { |s| assert_same_match(pat, s) }
  no_inputs.each  { |s| assert_same_match(pat, s) }
end

# =====================================================================
# Anchors
# =====================================================================

category "anchors" do
  sweep(/\Afoo/,    %w[foo foobar foo!],         ["xfoo", "", " foo", "foox\nfoo"])
  sweep(/foo\z/,    %w[foo xfoo afoo],           ["foox", "foo\n", "foobar"])
  sweep(/foo\Z/,    ["foo", "xfoo", "foo\n"],    ["foox", "foo\nbar"])
  sweep(/^foo/,     ["foo", "x\nfoo", "foo!"],   ["xfoo", " foo"])
  sweep(/foo$/,     ["foo", "afoo", "foo\nbar"], ["foox"])
  sweep(/^foo$/,    ["foo", "x\nfoo\nbar"],      ["foo bar", "x foo"])

  # Combined anchors
  sweep(/\A\d+\z/,  ["123", "0"],                ["123x", "x123", "12 3"])
  sweep(/^$/,       ["", "a\n\nb"],              ["a", " "])

  # Word boundaries
  sweep(/\bcat\b/,  ["cat", "the cat sat", "cat!"], ["cats", "scat", "subcat"])
  sweep(/\Bcat\B/,  ["scatter", "concatenate"],     ["cat", " cat "])

  # ASCII-only \w semantic for \b
  assert_same_match(/\bword\b/, "word_x")           # `_` is word char
  assert_same_match(/\bword\b/, "word.x")           # `.` isn't
end

# =====================================================================
# Quantifiers (greedy/lazy/possessive — possessive falls back to greedy)
# =====================================================================

category "quantifiers" do
  # Star, plus, optional
  sweep(/a*/,       ["", "a", "aa", "bbb"],       [])  # always matches
  sweep(/a+/,       ["a", "aa", "xaab"],          ["", "bbb"])
  sweep(/a?/,       ["", "a", "aa"],              [])
  sweep(/a?b/,      ["b", "ab"],                  ["x"])

  # Bounded
  sweep(/a{0}/,     ["x"],                        [])
  sweep(/a{1}/,     ["a", "xax"],                 ["", "bbb"])
  sweep(/a{3}/,     ["aaa", "aaaa", "xaaay"],     ["aa", "a", ""])
  sweep(/a{2,4}/,   ["aa", "aaa", "aaaa", "aaaaa"], ["a", ""])
  sweep(/a{2,}/,    ["aa", "aaa", "aaaaaaaa"],    ["a", ""])

  # Greedy vs lazy
  assert_same_match(/a.*b/,  "axxbxxb")           # greedy → 0..7
  assert_same_match(/a.*?b/, "axxbxxb")           # lazy   → 0..4
  assert_same_match(/a.+b/,  "axb")
  assert_same_match(/a.+?b/, "axxxb")
  assert_same_match(/a{2,4}/,  "aaaaaa")          # greedy: 4
  assert_same_match(/a{2,4}?/, "aaaaaa")          # lazy: 2

  # Lazy + outer chain — backtracking interaction
  assert_same_match(/<.+?>/,  "<a><b><c>")        # 0..3
  assert_same_match(/<.+>/,   "<a><b><c>")        # 0..9 (greedy)

  # Anchored zero-width
  assert_same_match(/^/,      "abc")              # 0..0
  assert_same_match(/$/,      "abc")              # 3..3
  assert_same_match(/^.*$/,   "abc")              # 0..3
end

# =====================================================================
# Character classes
# =====================================================================

category "classes" do
  sweep(/[abc]/,    ["a", "b", "c", "xax"],         ["xyz", ""])
  sweep(/[a-z]+/,   ["abc", "ABCdefGHI"],           ["ABC", "123"])
  sweep(/[A-Z]/,    ["X", "abcD"],                  ["abc", "123"])
  sweep(/[^abc]/,   ["x", "1"],                     ["a", "b", "c", ""])
  sweep(/[^0-9]+/,  ["abc", "abc123", "x1"],        ["123", "0"])

  # Predefined
  sweep(/\d/,       ["1", "abc 5"],                 ["abc", " "])
  sweep(/\D+/,      ["abc", "x1"],                  ["123", ""])
  sweep(/\w+/,      ["hello_42", "  word  "],       ["  ", ""])
  sweep(/\W/,       [" ", "x.y", "!"],              ["abc", "_"])
  sweep(/\s+/,      ["   ", " \t\n"],               ["abc", ""])
  sweep(/\S+/,      ["abc", "  abc  "],             ["   ", ""])
  # Onigmo extension: \h / \H = hex-digit / non-hex-digit
  sweep(/\h+/,      ["DEADBEEF", "0x1A2B", "abc"],  ["xyz ghi", " "])
  sweep(/\H+/,      ["xyz", "ghi", "ABC"],          ["1234567890abcdefABCDEF"])
  sweep(/[\h_]+/,   ["12_AB_cd", "_AA_", "0_F"],    ["xyz"])
  sweep(/[^\h]+/,   ["xyz", "ghi"],                 ["123abcDEF"])

  # Escapes inside
  sweep(/[\d.]+/,   ["1.2.3", "v1"],                ["abc"])
  sweep(/[\\\/]/,   ['\\', '/'],                    ["a"])
  sweep(Regexp.new("[\xc3]".b), ["\xc3", "a\xc3b"], ["abc"])

  # Quantified class
  sweep(/[A-Z]{3,}/, ["HELLO", "ABCdef"],           ["AB", "abcDEFghi"])
  sweep(/[a-z]+\d+/, ["abc123", "x1"],              ["ABC123", "123"])

  # ] inside class needs escaping
  assert_same_match(/[\]a]/, "]")
  assert_same_match(/[\]a]/, "a")

  # \b inside a class is backspace 0x08, not word boundary.
  assert_same_match(/[\b]/,  "\b")
  assert_same_match(/[\b]/,  "x")
  assert_same_match(/a[\b]b/, "a\bb")
  assert_same_match(/a[\b]b/, "ab")
end

# =====================================================================
# POSIX bracket classes
# =====================================================================

category "posix" do
  sweep(/[[:alpha:]]+/,   ["abc", "XYZ", "MixedCase"],     ["123", "  ", "!@#"])
  sweep(/[[:digit:]]+/,   ["0", "12345", "v42"],           ["abc", " "])
  sweep(/[[:alnum:]]+/,   ["abc123", "X_Y"],               ["   ", "..."])
  sweep(/[[:upper:]]+/,   ["HELLO", "AbcDEF"],             ["abc", "123"])
  sweep(/[[:lower:]]+/,   ["abc", "abcDEF"],               ["ABC", "123"])
  sweep(/[[:xdigit:]]+/,  ["DEADBEEF", "0xff", "1A2B"],    ["GHI", "xyz"])
  sweep(/[[:space:]]+/,   [" ", "\t\n", " ab "],           ["abc", ""])
  sweep(/[[:blank:]]+/,   [" ", "\t", " \t "],             ["\n", "abc"])
  sweep(/[[:print:]]+/,   ["abc!", "Hello World"],         ["", "\x01"])
  sweep(/[[:graph:]]+/,   ["abc!", "x"],                   [" ", "\t", ""])
  sweep(/[[:cntrl:]]+/,   ["\x01\x02", "\x7f"],            ["abc", " "])
  sweep(/[[:punct:]]+/,   ["!.?", "...","a,b"],            ["abc", " "])
  sweep(/[[:word:]]+/,    ["foo_bar", "x1y"],              ["...", "  "])

  # Negated POSIX
  sweep(/[[:^digit:]]+/,  ["abc", "abc123"],               ["123"])
  sweep(/[[:^space:]]+/,  ["abc", "abc def"],              [" ", "\t"])
  sweep(/[[:^alpha:]]+/,  ["123", "  ", "!?"],             ["abc"])

  # Mixed POSIX and literal in same class
  sweep(/[[:digit:]_]+/,  ["123", "abc_123_xyz"],          ["xyz", " "])
  sweep(/[[:alpha:][:digit:]_]+/, ["foo_42", "X"],         ["...", " "])

  # Counts and match_all on POSIX classes
  assert_same_count(/[[:digit:]]/,    "abc 1 23 456 d")
  assert_same_count(/[[:alpha:]]+/,   "abc 12 def ghi 3")
  assert_same_match_all(/[[:upper:]]+/, "Hello World, NICE Day")
end

# =====================================================================
# Alternation
# =====================================================================

category "alternation" do
  sweep(/cat|dog/,        ["cat", "dog", "doggone"], ["fish", "cad"])
  sweep(/foo|bar|baz/,    ["foo", "bar", "baz"],     ["qux"])
  sweep(/^(yes|no)$/,     ["yes", "no"],             ["maybe", "yesno"])

  # Precedence: alt is lowest
  sweep(/^a|b$/,          ["a", "ab", "xb"],         ["xa"])  # ^a OR b$
  sweep(/^(a|b)$/,        ["a", "b"],                ["ab", "x"])

  # Backtracking through alt + suffix
  assert_same_match(/(a|ab)c/,    "abc")              # picks "ab" via backtrack
  assert_same_match(/(ab|a)c/,    "abc")              # picks "ab" eagerly
  assert_same_match(/(a|aa|aaa)b/,"aaab")             # tries longest in any order

  # Alt with shared suffix (DAG in our AST)
  sweep(/(GET|POST|PUT)\s/,  ["GET ", "POST x", "PUT y"], ["DELETE ", "Get "])
end

# =====================================================================
# Groups (capturing, non-capturing, named)
# =====================================================================

category "groups" do
  assert_same_captures(/(\w+)/,                    "hello")
  assert_same_captures(/(\w+)\s+(\w+)/,            "hello world")
  assert_same_captures(/(a)(b)(c)/,                "abc")
  assert_same_captures(/(\d+)-(\d+)-(\d+)/,        "2025-05-01")
  assert_same_captures(/(?<num>\d+)/,              "id=42")
  assert_same_captures(/(?<lo>[a-z]+)(?<hi>[A-Z]+)/, "abcDEF")

  # Non-capturing
  assert_same_match(/(?:abc)+/,                    "abcabcabc")
  assert_same_match(/(?:foo)bar/,                  "foobar")

  # Nested
  assert_same_captures(/((\w+)\s+(\w+))/,          "hello world")
  assert_same_captures(/(a(b(c)))/,                "abc")

  # Repeated group: group capture = LAST iteration
  assert_same_captures(/(\d+)+/,                   "123")

  # Optional group → nil capture
  assert_same_captures(/(a)?b/,                    "b")
  assert_same_captures(/(a)?b/,                    "ab")
end

# =====================================================================
# Backreferences
# =====================================================================

category "backref" do
  sweep(/(.)\1/,           ["aa", "abccd"],          ["ab", "abc"])
  sweep(/(\w+)\s+\1/,      ["foo foo", "hi hi yo"],  ["foo bar", "no dup"])
  sweep(/^(.+)\1$/,        ["abab", "xyzxyz"],       ["abc", "abab x"])
  sweep(/(['"]).*?\1/,     [%q('hi'), %q("hello")],  ["'broken\""])

  # Backref to optional / not-yet-set group
  # /(\w+)?-\1/ — \1 is required even if (\w+)? skipped (Ruby errors? matches?)
  # Skip — semantics ambiguous.
end

# =====================================================================
# Lookaround
# =====================================================================

category "lookaround" do
  sweep(/foo(?=bar)/,    ["foobar", "foobarx"],     ["foobaz", "foo"])
  sweep(/foo(?!bar)/,    ["foobaz", "foo"],         ["foobar"])
  sweep(/(?=\w)\d/,      ["abc1"],                  ["1abc"])  # lookahead doesn't consume

  # Multi-step lookahead
  assert_same_match(/(?=.{5})\w+/,  "hello world")  # at least 5 chars from here
  assert_same_match(/x(?=ab)(?=a)/, "xab")          # both lookaheads must hold
end

# =====================================================================
# Flags
# =====================================================================

category "flags" do
  # /i case-insensitive
  sweep(/Hello/i,   ["hello", "HELLO", "HeLLo"],    ["Hi"])
  sweep(/[A-Z]+/i,  ["abc", "MIX"],                 [])  # /i widens classes
  sweep(/cat/i,     ["CAT", "cat", "Cat", "xCATy"], ["dog"])

  # /m multiline (dot matches \n)
  sweep(/a.c/m,     ["a\nc", "abc", "a c"],         ["ax"])
  sweep(/a[^q]+c/m, ["a\nb\nc"],                    ["aqqc"])

  # /x extended
  sweep(/a b c/x,   ["abc", "xabcy"],               ["a b c"])
  sweep(/\d \. \d/x, ["1.2"],                       ["1 . 2"])

  # /i + /m combined
  sweep(/\AHELLO.*\z/im, ["hello\nworld"],          ["goodbye"])
end

# =====================================================================
# Realistic patterns
# =====================================================================

category "realistic" do
  # IPv4 (lazy version, not strict)
  ipv4 = /(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})/
  sweep(ipv4, ["192.168.1.1", "ip 10.0.0.1 here"], ["1.2.3", "x.y.z"])
  assert_same_captures(ipv4, "lo 127.0.0.1 nh")

  # Email-ish (simplified, not RFC-correct)
  email = /(\w+)@(\w+\.\w+)/
  sweep(email, ["user@host.com", "a@b.c"],          ["@host.com", "user@", "noatsign"])

  # HTTP request line
  http = /^(GET|POST|PUT|DELETE) (\S+) HTTP\/(\d+\.\d+)/
  sweep(http, ["GET / HTTP/1.1", "POST /api HTTP/2.0"], ["INVALID", "GET no-version"])

  # C identifier
  ident = /\b[a-zA-Z_]\w*\b/
  assert_same_match_all(ident, "int main(int argc, char *argv[])")

  # Hex literal
  hex = /\b0x[0-9a-fA-F]+\b/
  sweep(hex, ["addr 0xDEADBEEF", "0x42 ok"],        ["DEADBEEF", "0xZZ"])

  # Quoted string (no escape handling)
  qstr = /"[^"]*"/
  sweep(qstr, [%q("hello"), %q(say "hi" yo)],       ["no quotes", '"unclosed'])
  assert_same_match_all(qstr, %q(a "x" b "y" c "z" end))

  # Parenthesized list (3 items)
  triple = /\((\w+),\s*(\w+),\s*(\w+)\)/
  assert_same_captures(triple, "f(a, b, c)")
  assert_same_captures(triple, "g( aa , bb , cc )")
end

# =====================================================================
# UTF-8 basics
# =====================================================================

category "utf-8" do
  # /é+/ deliberately tested with single-codepoint inputs only:
  # `é+` parses differently in Ruby /n mode vs astrogre — Ruby quantifies
  # only the last byte (\xa9+) of the codepoint; astrogre quantifies the
  # whole codepoint.  Both are defensible; we just don't compare on
  # multi-codepoint inputs.
  sweep(/é+/,         ["héllo", "café"],             ["hello", "abc"])
  sweep(/ü/,          ["über", "Müller"],           ["uber"])
  sweep(Regexp.new("[\xc3-\xc5]".b), ["\xc3", "\xc4 ", "\xc5"], ["abc"])

  # dot consumes whole codepoint
  assert_same_match(/./, "x")                       # 0..1 (ascii)
  # multi-byte dot edge — astrogre returns codepoint span; Ruby in
  # ASCII-8BIT also returns byte span via /n flag; skip /./ on
  # multibyte input since modes diverge by design.
end

# =====================================================================
# Edge cases
# =====================================================================

category "edge" do
  # Empty pattern matches at every position
  assert_same_match(/(?:)/,   "abc")                 # 0..0
  assert_same_match(/^/,      "abc")
  assert_same_match(/$/,      "abc")

  # Empty input
  assert_same_match(/.*/,     "")
  assert_same_match(/^$/,     "")
  assert_no_match(/./, "")

  # Very long input
  long = "x" * 1000 + "needle" + "y" * 1000
  assert_same_match(/needle/, long)
  assert_same_match(/^x+/,    long)

  # Many alternations
  assert_same_match(/a|b|c|d|e|f|g|h/, "ggg")

  # Zero-width quantifier interaction
  assert_same_match(/a*/,     "")                    # 0..0
  # NOTE: /(a*)*/ deliberately not tested — pathological nested empty-
  # match quantifier that requires explicit Ruby/PCRE-style "skip when
  # empty body matches" semantics; astrogre doesn't implement that
  # carve-out, so we'd recurse forever on the empty input.
end

# =====================================================================
# match_all enumeration
# =====================================================================

category "match_all" do
  assert_same_match_all(/\d+/,         "a1 b22 c333 d")
  assert_same_match_all(/[A-Z]/,       "aBcDeFgH")
  assert_same_match_all(/\b\w+\b/,     "the quick brown fox")
  assert_same_match_all(/foo/,         "foofoofoo")
  assert_same_match_all(/.../,         "abcdefgh")          # length-3 windows
  assert_same_match_all(/[aeiou]+/,    "hello beautiful world")
  assert_same_match_all(/^\w+/,        "alpha\nbeta\ngamma")
end

# =====================================================================
# count
# =====================================================================

category "count" do
  assert_same_count(/\d+/,      "abc 1 23 456 d")
  assert_same_count(/\w+/,      "hello world foo bar")
  assert_same_count(/(?:foo)+/, "foofoo bar foo baz foofoofoo")
  assert_same_count(/[a-z]/,    "ABCdefGHIjkl")
  assert_same_count(/^\s*#/,    "no\n  # yes\n# yep\nno comment")
  assert_same_count(/\b[A-Z]+\b/, "HELLO world ABC def XY ZZ")
  assert_same_count(/\.[a-z]+/, "main.c head.h util.cpp data.txt")
end

# =====================================================================
# AST shape introspection (using Pattern#dump)
# =====================================================================

category "dump-ast" do
  # Pure literal → memmem scanner with anchored_bos = 0
  assert_dump_contains(/static/,      "node_grep_search_memmem")
  assert_dump_contains(/static/,      '"static" 6 0')

  # \A literal → memmem with trailing anchored_bos=1 flag in dump
  d = compile_pat(/\Astatic/).dump
  if d.include?("node_re_bos") && d.match?(/ 1\)\s*$/)
    record_pass("dump /\\Astatic/ has BOS marker + anchored_bos=1")
  else
    record_fail("dump /\\Astatic/", "expected node_re_bos and trailing ` 1)`, got #{d.strip.inspect}")
  end

  # Single-byte first → memchr scanner
  assert_dump_contains(/x/,           "node_grep_search_memchr")

  # Class-led → class_scan or range scanner
  d = compile_pat(/[a-z]+/).dump
  if d.include?("node_grep_search_range") || d.include?("node_grep_search_class_scan")
    record_pass("dump /[a-z]+/ uses range or class scanner")
  else
    record_fail("dump /[a-z]+/", "expected range/class scanner, got #{d.strip.inspect}")
  end

  # Alt of literals → byteset
  assert_dump_contains(/cat|dog|bird/, "node_grep_search_byteset")
end

# =====================================================================
# API surface — non-match cases for each method
# =====================================================================

category "api" do
  re = compile_pat(/needle/)

  # match? returns plain bool
  if re.match?("haystack with needle") == true
    record_pass("match? returns true")
  else
    record_fail("match? returns true", "got non-true")
  end
  if re.match?("haystack") == false
    record_pass("match? returns false")
  else
    record_fail("match? returns false", "got non-false")
  end

  # match returns nil on miss
  if re.match("nope").nil?
    record_pass("match returns nil")
  else
    record_fail("match returns nil", "got non-nil")
  end

  # match returns ASTrogre::MatchData on hit
  md = re.match("a needle here")
  if md.is_a?(ASTrogre::MatchData) && md[0] == "needle" && md.byteoffset(0) == [2, 8]
    record_pass("match returns MatchData")
  else
    record_fail("match returns MatchData", "got #{md.inspect}")
  end

  # =~ returns position
  if (compile_pat(/needle/) =~ "xx needle yy") == 3
    record_pass("=~ returns position")
  else
    record_fail("=~", "got non-3")
  end

  # === for case statements
  if compile_pat(/^GET/) === "GET /"
    record_pass("=== matches")
  else
    record_fail("=== matches", "")
  end

  # source / options / names
  re3 = ASTrogre.compile(/(?<x>\d+)/i)
  re3.source == "(?<x>\\d+)" ? record_pass("source") : record_fail("source", re3.source)
  (re3.options & ASTrogre::Pattern::IGNORECASE) != 0 ? record_pass("options /i") : record_fail("options", re3.options.to_s)
  re3.names == ["x"]   ? record_pass("names")     : record_fail("names",     re3.names.inspect)
  re3.casefold?        ? record_pass("casefold?") : record_fail("casefold?", "")

  # match_all on no-match → [] (still an Array, just empty)
  if compile_pat(/zzz/).match_all("abc") == []
    record_pass("match_all empty on no match")
  else
    record_fail("match_all empty on no match", "non-empty")
  end

  # count on no-match → 0
  if compile_pat(/zzz/).count("abc") == 0
    record_pass("count = 0 on no match")
  else
    record_fail("count = 0 on no match", "non-zero")
  end

  # Compile with Regexp.new(...) wrappers
  if ASTrogre.compile(Regexp.new("abc")).match?("xabcy")
    record_pass("compile(Regexp.new(...))")
  else
    record_fail("compile(Regexp.new(...))", "no match")
  end

  # String pattern
  if ASTrogre.compile("abc").match?("xabcy")
    record_pass("compile(String)")
  else
    record_fail("compile(String)", "no match")
  end

  # Bad pattern → ArgumentError.  Silence the engine's stderr "regex
  # parse error: …" message so the test output stays clean.
  begin
    orig_stderr = $stderr
    $stderr = File.open(File::NULL, "w")
    ASTrogre.compile("(unclosed")
    record_fail("bad pattern raises", "no exception")
  rescue ArgumentError
    record_pass("bad pattern raises ArgumentError")
  ensure
    $stderr = orig_stderr if orig_stderr
  end
end

# =====================================================================
# Empty-body quantifier carve-out — patterns whose repeated body can
# match the empty string.  Ruby/PCRE both stop iterating the moment the
# body succeeds without advancing pos; without that rule the matcher
# would loop forever.  These tests just verify we agree with Ruby on
# the answer (and, more importantly, that we terminate at all).
# =====================================================================

# ---------------------------------------------------------------------
# Captures and named-captures API.  The Cext exposes:
#   Pattern#captures(input)        like MatchData#captures (positional)
#   Pattern#named_captures(input)  like MatchData#named_captures (hash)
#   Pattern#named_groups           static name → idx introspection
# Compared against MatchData on the byte-encoded regex.
# ---------------------------------------------------------------------

category "match_data" do
  # Pattern#match_data wraps the position result in an ASTrogre::Match
  # that returns substrings on `[]`, plus to_a / captures /
  # named_captures / pre_match / post_match — what callers usually want.

  def assert_match_strings(pat, input, expected_to_a)
    md = ASTrogre.compile(pat).match(input)
    if md.to_a == expected_to_a
      record_pass("match_data #{pat.inspect} / #{input.inspect}")
    else
      record_fail("match_data #{pat.inspect}", "got #{md.to_a.inspect}")
    end
  end

  assert_match_strings(/(\w+)\s+(\w+)/,                "hello world",
                       ["hello world", "hello", "world"])
  assert_match_strings(/(?<host>\w+):(?<port>\d+)/,    "ex:8080",
                       ["ex:8080", "ex", "8080"])
  assert_match_strings(/(a)(b)(c)/,                    "abc",
                       ["abc", "a", "b", "c"])

  # [] with int / Symbol / String
  md = ASTrogre.compile(/(?<host>\w+):(?<port>\d+)/).match("ex:8080")
  md[0]      == "ex:8080" ? record_pass("md[0]")        : record_fail("md[0]",        md[0].inspect)
  md[1]      == "ex"      ? record_pass("md[1]")        : record_fail("md[1]",        md[1].inspect)
  md[:host]  == "ex"      ? record_pass("md[:host]")    : record_fail("md[:host]",    md[:host].inspect)
  md["port"] == "8080"    ? record_pass("md[\"port\"]") : record_fail("md[\"port\"]", md["port"].inspect)
  md[99]     .nil?        ? record_pass("md[99] nil")   : record_fail("md[99]",       md[99].inspect)
  md[:nope]  .nil?        ? record_pass("md[:nope]")    : record_fail("md[:nope]",    md[:nope].inspect)

  # captures / named_captures (string form)
  md.captures        == ["ex", "8080"]                  ? record_pass("md.captures")        : record_fail("md.captures", md.captures.inspect)
  md.named_captures  == {"host" => "ex", "port" => "8080"} ? record_pass("md.named_captures") : record_fail("md.named_captures", md.named_captures.inspect)

  # begin / end
  md.begin(0) == 0 && md.end(0) == 7  ? record_pass("md.begin/end(0)")     : record_fail("md.begin/end(0)", "")
  md.begin(:host) == 0 && md.end(:host) == 2 ? record_pass("md.begin/end(:host)") : record_fail("md.begin/end(:host)", "")

  # pre_match / post_match
  md2 = ASTrogre.compile(/world/).match("hello world!")
  md2.pre_match  == "hello "  ? record_pass("pre_match")  : record_fail("pre_match",  md2.pre_match.inspect)
  md2.post_match == "!"       ? record_pass("post_match") : record_fail("post_match", md2.post_match.inspect)

  # No match → nil
  ASTrogre.compile(/x/).match("yyy").nil? ? record_pass("no match → nil") : record_fail("no match", "")

  # MatchData class shape
  md3 = ASTrogre.compile(/(?<a>\w+)/).match("hello")
  md3.is_a?(ASTrogre::MatchData) ? record_pass("MatchData class") : record_fail("MatchData class", md3.class.inspect)
  md3.names == ["a"]             ? record_pass("MatchData#names")  : record_fail("MatchData#names", md3.names.inspect)
  md3.string == "hello"          ? record_pass("MatchData#string") : record_fail("MatchData#string", md3.string.inspect)
  md3.regexp.is_a?(ASTrogre::Pattern) ? record_pass("MatchData#regexp") : record_fail("MatchData#regexp", "")
  md3.offset(0) == [0, 5]        ? record_pass("MatchData#offset") : record_fail("MatchData#offset", md3.offset(0).inspect)
  md3.values_at(0, :a) == ["hello", "hello"] ? record_pass("MatchData#values_at") : record_fail("MatchData#values_at", "")
  md3[0..1] == ["hello", "hello"] ? record_pass("MatchData#[range]") : record_fail("MatchData#[range]", "")

  # match_all
  all = ASTrogre.compile(/(\w+)/).match_all("foo bar baz")
  all.map(&:to_s) == ["foo", "bar", "baz"] ? record_pass("match_all") : record_fail("match_all", all.map(&:to_s).inspect)

  # Block form
  collected = []
  ASTrogre.compile(/(?<n>\d+)/).match_all("a1 b22 c333") { |m| collected << m[:n] }
  collected == ["1", "22", "333"] ? record_pass("match_all block") : record_fail("match_all block", collected.inspect)

  # Pattern matching (deconstruct / deconstruct_keys)
  case ASTrogre.compile(/(\w+)\s+(\w+)/).match("hello world")
  in [whole, a, b]
    whole == "hello world" && a == "hello" && b == "world" \
      ? record_pass("deconstruct (array pattern)")
      : record_fail("deconstruct", "got #{whole.inspect}, #{a.inspect}, #{b.inspect}")
  end
  case ASTrogre.compile(/(?<host>\w+):(?<port>\d+)/).match("ex:8080")
  in {host: h, port: p}
    h == "ex" && p == "8080" ? record_pass("deconstruct_keys (hash pattern)") : record_fail("deconstruct_keys", "")
  end
end

category "captures-api" do
  # Pattern#captures(input) and Pattern#named_captures(input) return
  # substrings (Ruby Regexp-compatible).  Compare to Ruby MatchData.
  def assert_same_captures_method(pat, input)
    md  = byte_regexp(pat).match(input.b)
    got = compile_pat(pat).captures(input.b)
    if md.nil?
      got.nil? ? record_pass("captures(nil) #{pat.inspect}") :
                 record_fail("captures #{pat.inspect}", "ruby=nil astrogre=#{got.inspect}")
      return
    end
    expected = (1..md.size - 1).map { |i| md[i] }   # substrings
    if expected == got
      record_pass("captures #{pat.inspect} / #{input.inspect}")
    else
      record_fail("captures #{pat.inspect}", "ruby=#{expected.inspect} astrogre=#{got.inspect}")
    end
  end

  def assert_same_named_captures(pat, input)
    md  = byte_regexp(pat).match(input.b)
    got = compile_pat(pat).named_captures(input.b)
    if md.nil?
      got.nil? ? record_pass("named_captures(nil) #{pat.inspect}") :
                 record_fail("named_captures #{pat.inspect}", "ruby=nil astrogre=#{got.inspect}")
      return
    end
    expected = byte_regexp(pat).named_captures.each_with_object({}) do |(name, idxs), h|
      i = idxs.last
      h[name] = md[i]
    end
    if expected == got
      record_pass("named_captures #{pat.inspect} / #{input.inspect}")
    else
      record_fail("named_captures #{pat.inspect}", "ruby=#{expected.inspect} astrogre=#{got.inspect}")
    end
  end

  # Positional captures
  assert_same_captures_method(/(\w+)\s+(\w+)/,    "hello world")
  assert_same_captures_method(/(\w+)\s+(\w+)/,    "single")
  assert_same_captures_method(/(a)(b)(c)/,        "abc")
  assert_same_captures_method(/(\d+)-(\d+)/,      "2025-05-01")
  assert_same_captures_method(/(a)?b/,            "b")
  assert_same_captures_method(/(a)?b/,            "ab")
  assert_same_captures_method(/x/,                "yyy")
  assert_same_captures_method(/(\w)(\w)(\w)/,     "xy")

  # Named captures
  assert_same_named_captures(/(?<n>\d+)/,                    "id=42")
  assert_same_named_captures(/(?<lo>[a-z]+)(?<hi>[A-Z]+)/,   "abcDEF")
  assert_same_named_captures(/(?<a>x)?(?<b>y)?/,             "xy")
  assert_same_named_captures(/(?<word>\w+)/,                 "")
  assert_same_named_captures(/(?<host>[\w.]+):(?<port>\d+)/, "example.com:8080")
  assert_same_named_captures(/(\w+)@(?<dom>\w+)/,            "u@h")

  # named_groups static introspection
  ng = ASTrogre.compile(/(?<lo>[a-z]+)(?<hi>[A-Z]+)/).named_groups
  ng == {"lo" => 1, "hi" => 2} ? record_pass("named_groups") : record_fail("named_groups", "got #{ng.inspect}")
  ASTrogre.compile(/\d+/).named_groups == {} ? record_pass("named_groups empty") :
                                               record_fail("named_groups empty", "non-empty")

  # match() returns MatchData; check shape
  full = ASTrogre.compile(/(?<n>\d+)/).match("id=42")
  full.is_a?(ASTrogre::MatchData) && full[0] == "42" && full[:n] == "42" ?
    record_pass("match → MatchData with named") :
    record_fail("match shape", "got #{full.inspect}")
end

category "keep-anchor" do
  # \K resets the whole-match start to the current pos; what came before
  # is consumed but not reported in the span.  Captures 1..N are kept.
  assert_same_match(/foo\Kbar/,             "foobar")
  assert_same_match(/foo\Kbar/,             "foobaz")
  assert_same_match(/foo\Kbar/,             "fobar")
  assert_same_match(/(\w+)\K=\w+/,          "key=val")
  assert_same_match(/abc\K/,                "xabc")
  assert_same_match(/abc\K/,                "xy")
  assert_same_match(/^\s*\K\w+/,            "    hello world")
  assert_same_match(/[A-Z]+\K\d+/,          "ABC123")

  # Captures unchanged
  assert_same_captures(/(\w+)\K=(\w+)/,     "key=val")
  assert_same_captures(/(\w+)\K=(\w+)/,     "no-equals")

  # Composition with alt and rep
  assert_same_match(/(?:foo|bar)\Kbaz/,     "foobaz")
  assert_same_match(/(?:foo|bar)\Kbaz/,     "barbaz")
  assert_same_match(/(?:foo|bar)\Kbaz/,     "quxbaz")
  assert_same_match(/^a+\K/,                "aaab")

  assert_same_count(/\w\K\d/,               "a1 b2 c3 d")
  assert_same_match_all(/\w\K\d/,           "a1 b2 c3 d")
end

category "subroutine-recursion" do
  # `(?<paren>\((?:[^()]|\g<paren>)*\))` — classic balanced-paren via
  # recursive subroutine.  Ruby's Onigmo accepts this too; we re-do it
  # via string-pattern compile in case `(?<…>)` confuses the host.
  paren = ASTrogre.compile('(?<paren>\((?:[^()]|\g<paren>)*\))')
  [
    ["(simple)",          "(simple)"],
    ["(nested(inner))",   "(nested(inner))"],
    ["((deep)(more))",    "((deep)(more))"],
    ["(((triple)))",      "(((triple)))"],
    ["(unclosed",         nil],
    ["no parens",         nil],
    ["leading (a) tail",  "(a)"],
  ].each do |input, expected|
    md = paren.match(input)
    got = md ? md[0] : nil
    if got == expected
      record_pass("paren #{input.inspect}")
    else
      record_fail("paren #{input.inspect}", "expected #{expected.inspect} got #{got.inspect}")
    end
  end

  # Optional self-reference: was previously rejected ("recursive
  # subroutine not supported"); now permitted.
  re_opt = ASTrogre.compile('(?<x>\g<x>?a)')
  re_opt.match("aaa")[0] == "aaa" ? record_pass("optional self-ref") :
                                     record_fail("optional self-ref", "")
end

category "subroutine" do
  # \g<name> / \g<N> re-evaluates the named/numbered group's pattern.
  # Note: NOT a backreference — it re-runs the regex, so different bytes
  # are matched.  Captures get re-set on each call.
  sweep(/(?<x>\d+)-\g<x>/,         ["12-34", "1-2"],            ["12-x"])
  sweep(/(?<x>[a-z]+)\W\g<x>/,     ["foo bar"],                 ["foo"])
  sweep(/(\w+),\g<1>/,             ["a,b", "hello,world"],      [","])
  sweep(/(?<lo>[a-z])(?<hi>[A-Z])\g<lo>\g<hi>/, ["aBcD"],       ["aB", "abcd"])
  assert_same_captures(/(?<x>\d+)-\g<x>/, "12-34")    # group 1 = LAST run ("34")

  # Subroutine inside count / match_all
  assert_same_match_all(/(?<x>\d+)-\g<x>/, "12-34 56-78 zz-99 100-200")
  assert_same_count(/(?<x>\d+)-\g<x>/,    "12-34 56-78 zz-99 100-200")
end

category "conditional" do
  # `(?(N)YES|NO)` — branch on whether group N captured.  Numbered and
  # named conditionals are both supported.
  sweep(/(a)?(?(1)b|c)/,            ["ab", "c"],                ["xyz"])
  sweep(/(?<x>a)?(?(<x>)b|c)/,      ["ab", "c"],                ["zz"])
  sweep(/(\d)?(?(1)X|Y)/,           ["1X", "Y", "5X"],          [])
  sweep(/(?:(<)|<)?(?(1)>|END)/,    ["<>", "<END", "END"],      [])

  # YES branch only (no NO branch defaults to empty)
  sweep(/(a)(?(1)b)c/,              ["abc"],                    ["ac"])

  # Captures preserved through whichever branch ran
  assert_same_captures(/(a)?(?(1)b|c)/, "ab")
  assert_same_captures(/(a)?(?(1)b|c)/, "c")
  assert_same_captures(/(a)?(?(1)(b)|(c))/, "ab")
  assert_same_captures(/(a)?(?(1)(b)|(c))/, "c")
end

category "atomic" do
  # (?>BODY) — body matches then commits.  If the outer continuation
  # later fails, no backtrack into body.
  sweep(/(?>a*)b/,            ["aaab", "b", "ab"],          ["aaa", "x"])
  sweep(/(?>a*)a/,            [],                            ["aaa", "a"])  # atomic ate everything
  sweep(/(?>foo|foob)ar/,     ["foobar"],                   ["foob"])      # cf. without atomic, foobar would match via "foo"+"bar" or "foob"+"ar"
  sweep(/(?>\d+)\.\d/,        ["1.2"],                       ["1"])
  sweep(/(?>[A-Z]+)[a-z]/,    ["Ab", "ABCd"],                ["abc"])
  sweep(/(?>x?)y/,            ["xy", "y"],                   ["x", ""])

  # Atomic is captureless
  assert_same_match(/(?>(?:a|b)+)c/, "aaabc")
  assert_same_match(/(?>(?:a|b)+)c/, "aaab")        # tail c missing → nil
end

category "possessive" do
  # Possessive quantifiers — desugar to atomic + greedy.
  sweep(/a*+b/,        ["aaab", "b", "ab"],            ["aaa", "x"])
  sweep(/a*+a/,        [],                              ["aaa", "a"])      # ate everything
  sweep(/a++/,         ["a", "aaa"],                    ["", "bbb"])
  sweep(/a?+a/,        ["a"],                           ["aa", ""])         # a?+ eats one a, tail /a/ fails on second
  sweep(/[a-z]*+x/,    ["x"],                           ["abcx"])
  sweep(/\d{2,4}+x/,   ["12x", "1234x"],                ["12345x", "1x"])

  # Possessive prevents catastrophic backtracking on otherwise pathological
  # alt+quantifier patterns.  Just verify these terminate quickly and agree.
  assert_same_match(/(?:a+)+b/,      "aaaab")
  assert_same_match(/(?:a++)+b/,     "aaaab")
end

category "lookbehind" do
  # (?<=BODY)foo / (?<!BODY)foo — fixed-width body only.  Bodies that
  # vary in length (\w+, alt-with-different-lengths) are rejected by
  # both Ruby and astrogre at parse time.
  sweep(/(?<=foo)bar/,         ["foobar"],                ["xxbar", "barfoo"])
  sweep(/(?<=\$)\d+/,          ["$42 plus", "abc $7"],    ["42", "$abc"])
  sweep(/(?<=[A-Z])[a-z]+/,    ["Abc", "DEf"],            ["abc"])
  sweep(/(?<=ab|cd)ef/,        ["abef", "cdef"],          ["xyef", "ef"])
  sweep(/(?<=\d{3})x/,         ["123x"],                  ["12x", "ax"])
  sweep(/(?<!foo)bar/,         ["xbar", "Xbar", "bar"],   ["foobar"])
  sweep(/(?<!\d)x/,            ["x", " x", "ax"],         ["1x", "0x"])
  sweep(/(?<=\b)\w+/,          ["hello", "  word"],       [])
  sweep(/(?<=^)foo/,           ["foo", "foox"],           ["xfoo"])
  sweep(/(?<=\Aabc)\d+/,       ["abc123"],                ["xabc123", "abcxyz"])

  # Combined with captures
  assert_same_captures(/(?<=\$)(\d+)/,    "price=$42")
  assert_same_captures(/(?<!\\)"([^"]*)"/, %q(say "hi"))   # escaped quote shouldn't match
  assert_same_captures(/(?<=#)(\w+)/,     "tag=#mention")

  # match_all + count
  assert_same_match_all(/(?<=\W)\d+/,     "x 1 22 ,333")
  assert_same_count(/(?<=\d)\w/,          "a1b 2c 33d")

  # True variable-length lookbehind.  Ruby/Onigmo refuses these at parse
  # time, so we compare against a hand-rolled expectation rather than
  # round-tripping through Ruby's regex engine.
  def expect_var_lb(src, input, expected_match)
    re = ASTrogre.compile(src)
    md = re.match(input)
    got = md ? md[0] : nil
    if got == expected_match
      record_pass("var-lb #{src.inspect} / #{input.inspect}")
    else
      record_fail("var-lb #{src.inspect} / #{input.inspect}", "expected #{expected_match.inspect} got #{got.inspect}")
    end
  end

  expect_var_lb('(?<=\w+)\d',        "abc1",     "1")
  expect_var_lb('(?<=\w+)\d',        "1",        nil)        # need preceding \w
  expect_var_lb('(?<=\w+)\d',        "x12",      "1")
  expect_var_lb('(?<!\w+)x',         " x",       "x")
  expect_var_lb('(?<!\w+)x',         " ax",      nil)
  expect_var_lb('(?<=\d{1,3})x',     "99x",      "x")
  expect_var_lb('(?<=\d{1,3})x',     "x",        nil)
  expect_var_lb('(?<=\d+,)\d+',      "12,345",   "345")

  # Variable-length body via alt-of-fixed-width branches.  We desugar at
  # parse time into a chain of fixed-width lookbehinds, so this covers
  # common cases like "(?<=^|<sep>)token".
  sweep(/(?<=ab|cdef)X/,         ["abX", "cdefX"],          ["zzX", "X"])
  sweep(/(?<!ab|cdef)X/,         ["zzX", "X", "aabX"],      ["abX", "cdefX"])
  sweep(/(?<=\W|\A)\d+/,         ["1", "x 22", "/333"],     ["abc"])
  assert_same_match_all(/(?<=\W|\A)\d+/, "x 1 22 ,333 a4")
  assert_same_count(/(?<=\W|\A)\d+/,     "x 1 22 ,333 a4")
end

category "g-anchor" do
  # \G anchors at the position the search started from.  For a single
  # `match` call that's pos 0 (so \G == \A); for `match_all` /
  # `String#scan` it advances to the previous end, chaining matches
  # only as long as they're contiguous.
  assert_same_match(/\Gfoo/,        "foofoo")
  assert_same_match(/\Gfoo/,        "xfoofoo")
  assert_same_match(/\Gfoo/,        "")
  assert_same_match(/\G\d+/,        "123abc")
  assert_same_match(/\G\d+/,        "abc123")        # no match: digits not at start
  assert_same_match(/\G[a-z]/,      "x")
  assert_same_match(/\G[a-z]/,      "1x")            # no match
  assert_same_match(/\G(?:foo|bar)/,"foo")
  assert_same_match(/\G(?:foo|bar)/,"bar")

  # match_all chains on contiguous matches; the moment a match fails to
  # re-anchor at the previous end, enumeration stops.
  assert_same_match_all(/\G\d+/,         "123 456 789")
  assert_same_match_all(/\G\d+,?/,       "1,2,3,4")
  assert_same_match_all(/\G[A-Z]/,       "ABCdef")        # stops after lowercase
  assert_same_match_all(/\G./,           "abc")
  assert_same_count(/\G\d+,?/,           "1,2,3,4")
end

category "newline-R" do
  # \R matches the generic newline, preferring \r\n over the parts.
  assert_same_match(/\R/,                    "hello\nworld")
  assert_same_match(/\R/,                    "hello\r\nworld")
  assert_same_match(/\R/,                    "\rfoo")
  assert_same_match(/\R/,                    "no eol here")
  assert_same_match(/\R+/,                   "line1\r\n\nline2")
  assert_same_match(/\R+/,                   "trailing\n\n\n")
  assert_same_match(/x\Ry/,                  "x\ny")
  assert_same_match(/x\Ry/,                  "x\r\ny")
  assert_same_match(/x\Ry/,                  "xny")
  assert_same_count(/\R/,                    "a\nb\r\nc\rd")
  assert_same_match_all(/\R/,                "a\nb\r\nc\rd")
end

category "empty-rep" do
  sweep(/(a*)*/,         ["", "aaa", "bbb"],            [])
  sweep(/(?:)*/,         ["", "xyz", "abc"],            [])
  sweep(/(?:a*)+/,       ["", "aaa", "x"],              [])
  sweep(/(\w*)*/,        ["abc", "", "_42"],            [])
  sweep(/(?:|x)*/,       ["", "xx", "abc"],             [])
  sweep(/(a|)*/,         ["aaa", "", "b"],              [])
  sweep(/(?:a*)*b/,      ["b", "aab", "x"],             ["aaa"])
  sweep(/(?:\d*)+/,      ["123", "", "abc"],            [])

  # Captures from optional/empty bodies
  assert_same_captures(/(a*)*/,        "aaa")
  assert_same_captures(/(\w*)\s*(\w*)/, "hello world")
  assert_same_captures(/(\w*)\s*(\w*)/, "")
end

# =====================================================================
# Matrix — combinatorial coverage by composing prefix × body × suffix.
# Each axis enumerates a small set of pattern fragments; we build every
# regex from the cross product and run it against a battery of inputs
# that span the obvious hit / miss / boundary scenarios.  This catches
# bugs where individual fragments work fine but compose oddly (e.g.
# anchor + quantifier + word boundary interactions).
# =====================================================================

category "matrix" do
  prefixes = ["", "^", "\\b", "\\A", "(?:abc)?"]
  bodies   = ["x", "[a-z]", "[a-z]+", "[a-z]{2,4}", "\\d+", "(foo|bar)", "\\w+"]
  suffixes = ["", "$", "\\b", "\\z", "(?=\\W|\\z)"]

  inputs = [
    "x", "abc", "abc123", "foo", "bar", "foobar",
    "hello world", "  spaces  ", "abc\n123",
    "", "X", "0", "_underscore_",
    "abcabcabc", "FOO", "foo123bar",
    "edge.case-here", "\tindented",
  ]

  prefixes.product(bodies, suffixes).each do |pre, body, suf|
    src = pre + body + suf
    pat = begin
      Regexp.new(src)
    rescue RegexpError
      next  # skip illegal compositions like /^$/ etc that Ruby rejects
    end
    inputs.each { |s| assert_same_match(pat, s, label: "[#{src.inspect}] / #{s.inspect}") }
  end
end

# =====================================================================
# Lines — grep-shaped patterns against multi-line strings.  These
# exercise the interaction between ^/$ and embedded newlines, which is
# the hot path for the grep CLI itself.
# =====================================================================

category "lines" do
  multiline_inputs = [
    "alpha\nbeta\ngamma",
    "GET /\nPOST /api\nPUT /x",
    "# comment\nreal_line\n# another",
    "  indented\nnot_indented\n   also_indented",
    "k=v\nkey = val\nno_equals\nfoo=\n=bar",
    "TODO: fix\nDONE: ok\nTODO done\nplain",
  ]

  patterns = [
    /^GET/,        /^POST/,            /^PUT/,        /^DELETE/,
    /^#/,          /^[A-Z]+:/,         /^\s+\w+/,     /^\w+\s*=\s*\w+$/,
    /TODO/,        /\bDONE\b/,         /^\w+$/,
    /\b\w+@\w+/,   /^[A-Z]\w+:/,       /:\s*$/,       /^[^#]/,
    /^.{20,}$/,    /^[a-z]+\n/m,
  ]

  patterns.product(multiline_inputs).each do |pat, input|
    assert_same_match(pat, input)
    assert_same_count(pat, input)
    assert_same_match_all(pat, input)
  end
end

# =====================================================================
# Position — match offset must be correct mid-string.  Each pattern is
# embedded in synthetic strings of varying prefix/suffix lengths so we
# catch off-by-one bugs in the start-pos that interp and AOT both
# compute.
# =====================================================================

category "position" do
  needle_specs = [
    [/needle/,        "needle"],
    [/\d+/,           "12345"],
    [/foo|bar/,       "bar"],
    [/[A-Z]+/,        "HELLO"],
    [/\bword\b/,      " word "],   # leading/trailing space is non-word boundary
    [/(\w+)@(\w+)/,   "u@h"],
  ]

  prefixes = ["", " ", "  ", "x", "abc", "x" * 50, "x" * 200]
  suffixes = ["", " ", "z", "tail", "z" * 50]

  needle_specs.each do |pat, body|
    prefixes.product(suffixes).each do |pre, suf|
      input = pre + body + suf
      assert_same_match(pat, input,
        label: "#{pat.inspect} @ pre=#{pre.size} body=#{body.inspect} suf=#{suf.size}")
    end
  end
end

# =====================================================================
# Long inputs — the kinds of strings the grep CLI sees in real files.
# Tests that the engine doesn't fall over on multi-KB inputs and that
# scanners advance correctly past large no-match regions.
# =====================================================================

category "long" do
  big_text = (
    "lorem ipsum dolor sit amet\n" +
    ("filler line that doesn't match anything special\n" * 100) +
    "TODO: this is the marker line we want\n" +
    ("more filler more filler more filler\n" * 100) +
    "ERROR: oh no\n" +
    ("trailing garbage line\n" * 50)
  )

  patterns = [
    /TODO/, /ERROR/, /^TODO:/, /^ERROR/,
    /\bmarker\b/, /lorem/, /^lorem/, /\Alorem/,
    /\bfiller\b/, /^trailing/,
    /no-such-thing/,                    # definite miss
    /\bipsum\b/, /\bdolor\b/,
    /^[A-Z]+:/, /:\s+\w+/,
  ]

  patterns.each do |pat|
    assert_same_match(pat, big_text)
    assert_same_count(pat, big_text)
  end

  # Many short lines, repeated needle
  haystack = (("noise " * 20 + "needle " + "noise " * 20 + "\n") * 50)
  assert_same_match(/needle/, haystack)
  assert_same_count(/needle/, haystack)
  assert_same_match_all(/^[a-z ]+\n/, haystack)
end

# =====================================================================
# Fuzz — generated patterns × generated inputs.  Building blocks are
# chosen to stay inside the agreed regex subset (no nested empty-match
# quantifiers, no multi-byte bodies, no `\b` inside char classes), so
# every Ruby answer should be reproducible.  Seeded for determinism so
# CI failures are reproducible verbatim.
# =====================================================================

category "fuzz" do
  rng = Random.new(0xC0FFEE)

  atoms_lit   = %w[a b c d 0 1 2 _]
  atoms_class = ["[ab]", "[a-c]", "[a-z]", "[0-9]", "[A-Z]", "[^abc]", "\\d", "\\w", "\\s", "."]
  atoms_alt   = ["(a|b)", "(foo|bar)", "(?:x|y|z)"]

  qualifiers  = ["", "?", "*", "+", "{2}", "{1,3}"]

  build_pattern = lambda do |atom_count|
    parts = []
    atom_count.times do
      pool  = [atoms_lit, atoms_class, atoms_alt][rng.rand(3)]
      atom  = pool.sample(random: rng)
      qual  = qualifiers.sample(random: rng)
      qual  = "" if atom.start_with?("(")  # avoid `(?:x|y|z){1,3}` size blowups
      parts << atom + qual
    end
    src = parts.join
    src = "^" + src if rng.rand(4).zero?
    src = src + "$" if rng.rand(4).zero?
    src
  end

  build_input = lambda do
    alphabet = "abcd012XYZ_ "
    n = rng.rand(0..30)
    s = String.new(capacity: n)
    n.times { s << alphabet[rng.rand(alphabet.size)] }
    s
  end

  fuzz_n = ENV.fetch("FUZZ_N", "150").to_i
  attempted = 0
  while attempted < fuzz_n
    src = build_pattern.call(rng.rand(1..4))
    pat = begin
      Regexp.new(src)
    rescue RegexpError
      next  # invalid composition; try again
    end

    # Hit the same pattern with a few inputs — different lengths, with a
    # few biased toward containing matchable bytes.
    3.times do
      input = build_input.call
      assert_same_match(pat, input, label: "fuzz/#{src} / #{input.inspect}")
      assert_same_count(pat, input)
      attempted += 1
    end
  end
end

# =====================================================================
# Report
# =====================================================================

total = @pass + @fail
puts ""
puts "by category:"
@categories.each do |cat, ct|
  total_cat = ct[:pass] + ct[:fail]
  flag = ct[:fail].zero? ? " " : "!"
  printf("  %1s %-15s  %4d / %4d\n", flag, cat, ct[:pass], total_cat)
end
puts ""
puts "#{@pass}/#{total} passed#{@fail.zero? ? "" : "  (#{@fail} failed)"}  [#{AOT_MODE ? "aot" : "interp"}]"
exit(@fail.zero? ? 0 : 1)
