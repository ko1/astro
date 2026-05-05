#!/usr/bin/env ruby
# nuq test runner.
#
# Each test/*.test file uses the jq test format:
#   line 1: filter
#   line 2: input JSON (one line)
#   line 3+: expected output (one per line until blank)
# Tests are separated by blank lines.
#
# Lines starting with `#` are comments and skipped.
#
# We diff nuq's output against the expected lines.  If the file is
# named *.diff.test we use jq itself to generate the expected output
# (differential testing).

require 'open3'
require 'json'

ROOT = File.expand_path('..', __dir__)
NUQ  = File.join(ROOT, 'nuq')
abort "nuq binary missing — run `make` first" unless File.executable?(NUQ)

def parse_test_file(path)
  src = File.read(path)
  lines = src.lines.map(&:chomp)
  tests = []
  i = 0
  while i < lines.length
    while i < lines.length && (lines[i].strip.empty? || lines[i].lstrip.start_with?('#'))
      i += 1
    end
    break if i >= lines.length
    filter = lines[i]; i += 1
    while i < lines.length && lines[i].lstrip.start_with?('#')
      i += 1
    end
    break if i >= lines.length
    input = lines[i]; i += 1
    expected = []
    while i < lines.length && !lines[i].strip.empty?
      next i += 1 if lines[i].lstrip.start_with?('#')
      expected << lines[i]
      i += 1
    end
    tests << [filter, input, expected]
  end
  tests
end

def run_one(filter, input)
  out, err, st = Open3.capture3(NUQ, '-c', '--no-compile', filter,
                                stdin_data: input)
  [out, err, st]
end

def run_jq(filter, input)
  out, err, st = Open3.capture3('jq', '-c', filter, stdin_data: input)
  [out, err, st]
end

total = 0; pass = 0; fail = 0; skip = 0
failures = []

Dir["#{__dir__}/*.test"].sort.each do |path|
  base = File.basename(path)
  use_diff = base.end_with?('.diff.test')
  tests = parse_test_file(path)
  tests.each do |filter, input, expected|
    total += 1

    if use_diff
      jq_out, _, jq_st = run_jq(filter, input)
      unless jq_st.success?
        skip += 1
        next
      end
      expected = jq_out.lines.map(&:chomp)
    end

    nuq_out, nuq_err, nuq_st = run_one(filter, input)
    nuq_lines = nuq_out.lines.map(&:chomp)

    ok = nuq_st.success? && nuq_lines == expected
    if ok
      pass += 1
    else
      fail += 1
      failures << {
        file: base, filter: filter, input: input,
        expected: expected, actual: nuq_lines,
        err: nuq_err, exit: nuq_st.exitstatus
      }
    end
  end
end

puts ""
puts "passed: #{pass}  failed: #{fail}  skipped: #{skip}  total: #{total}"

if !failures.empty?
  puts ""
  puts "FAILURES:"
  failures.first(30).each do |f|
    puts "  [#{f[:file]}]"
    puts "    filter:   #{f[:filter]}"
    puts "    input:    #{f[:input]}"
    puts "    expected: #{f[:expected].inspect}"
    puts "    actual:   #{f[:actual].inspect}"
    if !f[:err].empty?
      puts "    stderr:   #{f[:err].lines.first&.chomp}"
    end
    puts ""
  end
  if failures.length > 30
    puts "... (#{failures.length - 30} more failures)"
  end
  exit 1
end
