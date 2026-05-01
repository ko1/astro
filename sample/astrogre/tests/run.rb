#!/usr/bin/env ruby
# Ruby-driven test suite for astrogre.
#
# Three layers of tests, all using Ruby's own Regexp / String / system
# grep as ground truth so we don't have to maintain expected values
# manually:
#
#   1. Engine-level (--via-prism + --dump): parse a regex, check the
#      AST shape we expect (smoke tests for parser / lower).
#   2. CLI-level: run `./astrogre [flags] PATTERN file` and diff against
#      `grep [flags] PATTERN file` byte-for-byte.
#   3. Ruby-vs-astrogre: build a small corpus, run the same regex in
#      Ruby and via astrogre's CLI, compare matched lines / counts.
#
# Run: ruby tests/run.rb
# Optional: ruby tests/run.rb --verbose

require "tmpdir"
require "open3"
require "fileutils"

ROOT      = File.expand_path("..", __dir__)
ASTROGRE  = File.join(ROOT, "astrogre")
GREP      = ENV["GREP"] || "/usr/bin/grep"

VERBOSE = ARGV.include?("--verbose")

class Counter
  attr_reader :pass, :fail
  def initialize; @pass = 0; @fail = 0; end
  def ok!(name)
    @pass += 1
    puts "  ok    #{name}" if VERBOSE
  end
  def fail!(name, msg)
    @fail += 1
    puts "  FAIL  #{name}: #{msg}"
  end
  def report
    total = @pass + @fail
    puts "\n#{@pass}/#{total} passed#{@fail.zero? ? "" : "  (#{@fail} failed)"}"
    @fail.zero?
  end
end

C = Counter.new

# Run a command, return [stdout, stderr, exit_status].
def run(*cmd)
  Open3.capture3(*cmd)
end

# ---------------------------------------------------------------------
# Layer 1: engine — pattern parses + dumps without crashing
# ---------------------------------------------------------------------

ENGINE_PATTERNS = [
  # literals
  "/foo/", "/Hello, World!/", "/abc123/",
  # anchors
  '/\Afoo/', '/foo\z/', '/foo\Z/', "/^foo/", '/foo$/',
  '/\bword\b/', '/\Bin\B/',
  # classes
  "/[a-z]/", "/[^0-9]/", '/\d+/', '/\w+/', '/\s+/',
  '/[\d\.]+/', "/[a-zA-Z_][a-zA-Z0-9_]*/",
  # quantifiers
  "/a*/", "/a+/", "/a?/", "/a{3}/", "/a{2,5}/", "/a{2,}/",
  "/a*?/", "/a+?/", "/a??/",
  # alternation
  "/cat|dog/", "/(cat|dog|bird)/",
  # groups
  "/(foo)+/", "/(?:foo)+/", '/(?<name>\d+)/',
  # backref
  '/(\w+)\s+\1/',
  # lookaround
  "/foo(?=bar)/", "/foo(?!bar)/",
  # flags
  "/Hello/i", "/a.c/m", "/a b c/x",
  # UTF-8 literals
  "/é+/", "/.*/",
  # combined
  '/^static\s+(\w+)\s*\(/', '/(\d+)\.(\d+)\.(\d+)/',
]

ENGINE_PATTERNS.each do |pat|
  out, err, st = run(ASTROGRE, "--dump", pat)
  if st.success? && !out.empty?
    C.ok!("engine parse #{pat}")
  else
    C.fail!("engine parse #{pat}", "exit=#{st.exitstatus} err=#{err.strip}")
  end
end

# ---------------------------------------------------------------------
# Layer 2: CLI vs grep — byte-identical output across many flag combos
# ---------------------------------------------------------------------

# Build a small corpus that exercises a lot of edge cases.
CORPUS = <<~CORPUS.dup
  static int hello;
  static const char *world = "static";
  inline void run(void);
  /* comment with static keyword */
   indented static line
  STATIC int upper;
  int main(void) {
      printf("hello\\n");
      return 0;
  }
  void func(int x) {}
  // line with no static
  static_var = 1;
  not-static
  prefix.static
  static
CORPUS

corpus_file = File.join(Dir.tmpdir, "astrogre_test_corpus.txt")
File.write(corpus_file, CORPUS)

# A variety of (description, flags, pattern) triples.  Each runs through
# both `astrogre` and `grep -E` and we compare stdout + exit-class
# (matched / no-match / error).
CLI_CASES = [
  ["literal",                [],            "static"],
  ["case-insensitive",       %w[-i],        "STATIC"],
  ["count",                  %w[-c],        "static"],
  ["invert",                 %w[-v],        "static"],
  ["whole-word",             %w[-w],        "static"],
  ["fixed-string",           %w[-F],        "static"],
  ["files-with-matches",     %w[-l],        "static"],
  ["files-without-match",    %w[-L],        "nonexistent"],
  ["only-matching",          %w[-o],        "static"],
  ["line-numbers",           %w[-n],        "static"],
  ["whole-line",             %w[-x],        "static"],
  ["max-count",              %w[-m 3],      "static"],
  ["max-count + count",      %w[-c -m 3],   "static"],
  ["context-after",          %w[-A 1],      "STATIC"],
  ["context-before",         %w[-B 1],      "STATIC"],
  ["context-both",           %w[-C 2],      "STATIC"],
  ["NUL separator + -l",     %w[-Z -l],     "static"],
  ["regex anchor ^",         [],            "^static"],
  ["regex anchor $",         [],            "static$"],
  ["regex alt",              [],            "static|inline"],
  ["regex class",            [],            "[A-Z]+"],
  ["regex class-rep",        [],            "[0-9]+"],
  ["regex \\b",              [],            '\bstatic\b'],
  ["regex group",            %w[-E],        '(static|inline) +(int|void)'],
  ["no match",               [],            "ZZZNOMATCH"],
  ["no match -c",            %w[-c],        "ZZZNOMATCH"],
]

CLI_CASES.each do |name, flags, pat|
  a_out, _, a_st = run(ASTROGRE, *flags, pat, corpus_file)
  # grep: prepend -E unless -F is in play (they're mutually exclusive).
  grep_flags = flags.dup
  grep_flags = ["-E"] + grep_flags unless grep_flags.include?("-F") || grep_flags.include?("-E")
  g_out, _, g_st = run(GREP,     *grep_flags, pat, corpus_file)
  outcome = ->(st) { st.exitstatus < 2 ? (st.exitstatus == 0 ? :match : :no_match) : :error }
  if outcome.call(a_st) != outcome.call(g_st)
    C.fail!("CLI #{name} `#{pat}`", "exit class differs astrogre=#{a_st.exitstatus} grep=#{g_st.exitstatus}")
    next
  end
  if a_out != g_out
    C.fail!("CLI #{name} `#{pat}`", "stdout differs (astrogre #{a_out.bytesize} bytes, grep #{g_out.bytesize} bytes)")
    if VERBOSE
      puts "    --- astrogre ---"; puts a_out.lines.first(5).join
      puts "    --- grep ---";     puts g_out.lines.first(5).join
    end
  else
    C.ok!("CLI #{name} `#{pat}`")
  end
end

# ---------------------------------------------------------------------
# Layer 3: Ruby Regexp vs astrogre — match / no-match should agree on
# many random patterns + inputs.  Captures aren't compared (output
# format differs); we just check that the line-level decision matches.
# ---------------------------------------------------------------------

RUBY_CASES = [
  # [pattern_str, [inputs that must match], [inputs that must NOT match]]
  ['hello',          ["hello world", "say hello"],  ["world", "hellp"]],
  ['^foo$',          ["foo"],                       ["foox", "xfoo"]],
  ['\d+',            ["abc 123", "x42"],            ["abc", "..."]],
  ['[A-Z]{3,}',      ["HELLO", "ABCdef"],           ["abc", "Ab"]],
  ['(cat|dog) food', ["cat food", "dog food yum"],  ["fish food", "catfood"]],
  ['\b\w+\b',        ["one two", "x"],              [""]],
  ['(\w+)@(\w+)',    ["a@b", "user@host"],          ["@only", "user@", "noat"]],
  ['^\s*#',          ["  # comment", "#root"],      ["x #", "no leading"]],
  ['\.(c|h)$',       ["main.c", "head.h"],          ["main.cpp", "data.txt"]],
  ['\Aint\s+\w+',    ["int x", "int  count = 0"],   [" int x", "uint x"]],
  # quantifier edge cases
  ['a{3}',           ["aaa", "aaaa", "xaaaay"],     ["aa", "a"]],
  ['a{0,2}b',        ["b", "ab", "aab"],            ["nothing here", "ccc"]],
  ['^.{5}$',         ["12345", "abcde"],            ["1234", "123456"]],
  # backref
  ['^(.+)\1$',       ["abab", "xx"],                ["abc", "abab "]],
  # alternation precedence
  ['a|b+',           ["a", "bbb", "ax"],            ["c", ""]],
  ['^(yes|no)$',     ["yes", "no"],                 ["yesx", "maybe"]],
  # literal w/ special chars
  ['.+\.txt$',       ["readme.txt", "a.txt"],       ["readmetxt", "a.txt2"]],
  ['\$\d+',          ["price $100"],                ["100", "$x"]],
  # UTF-8 basics
  ['é+',             ["héllo", "café"],             ["hello"]],
  # hex / unicode escapes
  ['\x41\x42',       ["AB", "xABx"],                ["ab", "AC"]],
  # group with quantifier
  ['(\d+\.){3}\d+',  ["192.168.1.1", "10.0.0.1"],   ["1.2.3", "1..2.3.4"]],
  # alternation with anchors
  ['^(GET|POST|PUT)\s',  ["GET /index", "POST /api"],  ["DELETE /x", "Get /lower"]],
]

RUBY_CASES.each_with_index do |(pat, yes_inputs, no_inputs), i|
  ruby_re = Regexp.new(pat)

  yes_inputs.each do |inp|
    # Ground truth: Ruby says it matches.
    raise "Ruby disagrees: #{pat.inspect} should match #{inp.inspect}" unless ruby_re =~ inp

    # astrogre via stdin.
    out, _, st = Open3.capture3(ASTROGRE, "-q", pat, stdin_data: inp)
    if st.exitstatus == 0
      C.ok!("Ruby vs astrogre yes #{pat.inspect} / #{inp.inspect}")
    else
      C.fail!("Ruby vs astrogre yes #{pat.inspect} / #{inp.inspect}",
              "astrogre exit=#{st.exitstatus} (Ruby matched)")
    end
  end

  no_inputs.each do |inp|
    raise "Ruby disagrees: #{pat.inspect} should NOT match #{inp.inspect}" if ruby_re =~ inp
    out, _, st = Open3.capture3(ASTROGRE, "-q", pat, stdin_data: inp)
    if st.exitstatus == 1
      C.ok!("Ruby vs astrogre no  #{pat.inspect} / #{inp.inspect}")
    else
      C.fail!("Ruby vs astrogre no  #{pat.inspect} / #{inp.inspect}",
              "astrogre exit=#{st.exitstatus} (Ruby did not match)")
    end
  end
end

# ---------------------------------------------------------------------
# Layer 4: --via-prism — the same Ruby source string should yield the
# same regex behaviour as Ruby's own Regexp.
# ---------------------------------------------------------------------

VIA_PRISM_CASES = [
  ['/\d+/',        "abc123def",  true],
  ['/\d+/',        "no digits",  false],
  ['/foo/i',       "FoO",        true],
  ['/(?<num>\d+)/', "x42y",      true],
  ['/^\s*$/',      "   ",        true],
]

VIA_PRISM_CASES.each do |ruby_src, input, should_match|
  out, _, st = Open3.capture3(ASTROGRE, "--via-prism", "-q", ruby_src, stdin_data: input)
  matched = (st.exitstatus == 0)
  if matched == should_match
    C.ok!("via-prism #{ruby_src.inspect} / #{input.inspect}")
  else
    C.fail!("via-prism #{ruby_src.inspect} / #{input.inspect}",
            "want #{should_match}, got #{matched} (exit=#{st.exitstatus})")
  end
end

FileUtils.rm_f(corpus_file)
exit(C.report ? 0 : 1)
