#!/usr/bin/env ruby
# Test runner: runs every test/*.r and compares the printed `Result:`
# (or, when an `# expect:` annotation is present, the corresponding
# stdout line) against the expected value.

require 'open3'

ROOT = File.expand_path('..', __dir__)
BIN  = File.join(ROOT, 'astr')
abort "binary missing: #{BIN} (run `make` first)" unless File.executable?(BIN)

pass = fail = 0
Dir.glob(File.join(__dir__, '*.r')).sort.each do |path|
  src = File.read(path)
  expect_line = src.lines.find { _1 =~ /\A#\s*expect:\s*(.*)/ }
  expect = expect_line ? expect_line[/\A#\s*expect:\s*(.*)/, 1].strip : nil

  out, err, status = Open3.capture3(BIN, '-q', path)
  # Strip R's `[1] ` print prefix so existing tests continue to compare
  # the underlying scalar value.
  actual = out.lines.first&.strip&.sub(/\A\[\d+\]\s*/, '')
  ok = expect && actual == expect && status.success?

  name = File.basename(path)
  if ok
    pass += 1
    puts "  pass  #{name.ljust(16)}  => #{actual}"
  else
    fail += 1
    puts "  FAIL  #{name.ljust(16)}  expect=#{expect.inspect} actual=#{actual.inspect}"
    puts err.lines.map { "        #{_1}" }.join unless err.empty?
  end
end

puts
puts "#{pass} passed, #{fail} failed"
exit(fail == 0 ? 0 : 1)
