#!/usr/bin/env ruby
# Run all jstro tests under test/.  Each test is expected to exit with
# code 0 and to print a final "ALL TESTS PASSED" / "ALL SPEC TESTS PASSED"
# style banner.

require 'open3'

dir = File.expand_path('..', __dir__)
tests = Dir["#{dir}/test/*.js"].sort

failed = 0
tests.each do |t|
  puts "==== #{File.basename(t)} ===="
  out, err, st = Open3.capture3("#{dir}/jstro", t)
  passed = st.success? && out =~ /ALL .*PASSED/
  if passed
    puts "  -> ok (#{out.lines.count} lines)"
  else
    puts "  FAIL  exit=#{st.exitstatus}"
    puts "  STDOUT (last 5 lines):"
    out.lines.last(5).each { |l| puts "    #{l.chomp}" }
    puts "  STDERR: #{err.chomp}"
    failed += 1
  end
end

puts
if failed == 0
  puts "All #{tests.size} test files pass."
  exit 0
else
  puts "#{failed} of #{tests.size} test files failed."
  exit 1
end
