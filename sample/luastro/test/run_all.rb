#!/usr/bin/env ruby
# luastro test runner.
#
# Each test_*.lua under test/ is a self-contained Lua program whose
# stdout output is compared against a sibling test_*.expected file.

require 'open3'

DIR = File.dirname(__FILE__)
EXE = File.expand_path("../luastro", DIR)
LUA54 = "lua5.4"

abort "luastro not built — run 'make' first" unless File.executable?(EXE)

tests = Dir["#{DIR}/test_*.lua"].sort
pass = 0
fail = 0
failed = []

tests.each do |t|
  expected_path = t.sub(/\.lua$/, ".expected")
  base = File.basename(t)

  if File.exist?(expected_path)
    expected = File.read(expected_path)
  else
    out, st = Open3.capture2(LUA54, t)
    expected = out
  end

  out, err, st = Open3.capture3(EXE, t)
  if st.success? && out == expected
    pass += 1
    printf "  OK  %s\n", base
  else
    fail += 1
    failed << base
    printf "FAIL  %s\n", base
    if !st.success?
      printf "      exit %d  err: %s\n", st.exitstatus.to_i, err.lines.first.to_s.chomp
    end
    if out != expected
      printf "      expected: %p\n", expected[0,80]
      printf "      got:      %p\n", out[0,80]
    end
  end
end

puts "---"
puts "passed: #{pass}  failed: #{fail}  total: #{tests.size}"
exit(fail == 0 ? 0 : 1)
