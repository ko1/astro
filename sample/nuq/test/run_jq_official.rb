#!/usr/bin/env ruby
# Run the jq project's official tests/jq.test against nuq and tally
# pass / fail / skip / error.  This is a compatibility scorecard, not
# a hard test gate (failures here are expected for known gaps:
# regex, dynamic-path assignment, modules, etc.).
#
# Format of jq.test groups (separated by blank lines, '#' is comment):
#   line 1:   filter
#   line 2:   input JSON (one line)
#   line 3+:  expected output JSON values, one per line
# A program prefixed with '%%FAIL' / '%%FAIL IGNORE MSG:' marks
# expected-error tests; we treat those as PASS if nuq also errors.
#
# Usage:
#   ruby test/run_jq_official.rb            # full suite
#   ruby test/run_jq_official.rb --verbose  # show every failing case
#   ruby test/run_jq_official.rb --first 50 # only first 50 cases

require 'open3'
require 'json'

ROOT = File.expand_path('..', __dir__)
NUQ  = File.join(ROOT, 'nuq')
TEST = ENV['JQ_TESTFILE'] || '/tmp/claude/jq.test'

abort "nuq missing — run `make`" unless File.executable?(NUQ)
abort "test file missing: #{TEST}" unless File.file?(TEST)

# Parse the file into [{filter, input, expected, fail_expected}, ...]
def parse_tests(path)
  tests = []
  File.read(path).split(/\n\n+/).each do |block|
    lines = block.lines.map(&:chomp).reject { |l| l.lstrip.start_with?('#') || l.strip.empty? }
    next if lines.empty?
    filter = lines.shift
    fail_expected = false
    if filter =~ /\A%%FAIL/
      fail_expected = true
      filter.sub!(/\A%%FAIL(?:\s+IGNORE\s+MSG)?\s*/, '')
      next if filter.empty?
    end
    next if lines.empty?
    input = lines.shift
    expected = lines
    tests << { filter: filter, input: input, expected: expected,
               fail_expected: fail_expected }
  end
  tests
end

VERBOSE = ARGV.include?('--verbose') || ARGV.include?('-v')
FIRST_N = (ARGV.find { |a| a =~ /^--first=(\d+)/ } || ARGV.find { |a| a =~ /^--first$/ } )
n_limit = nil
if (i = ARGV.index('--first')) && ARGV[i+1] =~ /\A\d+\z/
  n_limit = ARGV[i+1].to_i
end

tests = parse_tests(TEST)
tests = tests.first(n_limit) if n_limit
puts "Loaded #{tests.size} tests from #{TEST}"

pass = 0
fail = 0
err  = 0
skip = 0
failures = []

tests.each_with_index do |t, idx|
  out, err_msg, st = nil, nil, nil
  begin
    out, err_msg, st = Open3.capture3(NUQ, '-c', '--no-compile', t[:filter],
                                      stdin_data: t[:input])
  rescue => e
    err += 1
    failures << [idx, t, "exception: #{e.message}"]
    next
  end

  exited_ok = st && st.exited? && st.exitstatus == 0

  if t[:fail_expected]
    # Test expects jq to fail.  PASS if nuq also exits non-zero
    # OR prints an error to stderr.
    if !exited_ok || (err_msg && !err_msg.empty?)
      pass += 1
    else
      fail += 1
      failures << [idx, t, "expected fail, got success: #{out.inspect}"]
    end
    next
  end

  unless exited_ok
    # Could be a feature gap.  Classify as fail.
    fail += 1
    failures << [idx, t, "exit #{st&.exitstatus}: #{err_msg.lines.first&.chomp}"]
    next
  end

  actual = out.lines.map(&:chomp)
  # jq's `--run-tests` compares JSON values, not textual output, so
  # whitespace inside compact-ish forms (`{"a":1, "b":2}` vs
  # `{"a":1,"b":2}`) is irrelevant.  Parse both sides as JSON and
  # compare structurally.
  begin
    actual_v   = actual.map { |s| JSON.parse(s) }
    expected_v = t[:expected].map { |s| JSON.parse(s) }
  rescue JSON::ParserError => e
    fail += 1
    failures << [idx, t, "JSON parse error: #{e.message}; got #{actual.inspect}"]
    next
  end
  if actual_v == expected_v
    pass += 1
  else
    fail += 1
    failures << [idx, t, "expected #{t[:expected].inspect}, got #{actual.inspect}"]
  end
end

total = pass + fail + err + skip

puts ""
puts "=== jq official test results ==="
puts "  total:   #{total}"
puts "  pass:    #{pass}  (#{(pass.to_f / total * 100).round(1)}%)"
puts "  fail:    #{fail}"
puts "  error:   #{err}"
puts ""

if VERBOSE
  failures.each do |idx, t, msg|
    puts "--- ##{idx + 1} ---"
    puts "  filter:   #{t[:filter]}"
    puts "  input:    #{t[:input]}"
    puts "  expected: #{t[:expected].inspect}"
    puts "  failure:  #{msg}"
  end
elsif !failures.empty?
  # Categorize all failures by gap type
  puts "=== failure categories ==="
  cats = Hash.new(0)
  failures.each do |idx, t, msg|
    cat = case msg
          when /(\w+\/\d+) is not defined/ then "missing builtin: #{$1}"
          when /path expression: dynamic/ then "path-mode: dynamic index"
          when /path expression: not array/ then "path-mode: type"
          when /path expression: cannot iterate/ then "path-mode: iter type"
          when /path expression: unsupported/ then "path-mode: unsupported AST"
          when /Cannot iterate/ then "iter type error"
          when /Out of bounds/ then "out of bounds"
          when /parse error|parse: expected|nuq parse:/ then "parse error"
          when /Path too deep/ then "path validation"
          when /Cannot update/ then "setpath validation"
          when /containment/ then "contains type"
          when /^expected.*got/ then "value mismatch"
          when /JSON parse error/ then "non-JSON output"
          else msg[0, 50]
          end
    cats[cat] += 1
  end
  cats.sort_by { |_, n| -n }.each { |k, n| printf "  %4d  %s\n", n, k }
  puts ""
  puts "(use --verbose for full per-test output, --first N to limit)"
end
