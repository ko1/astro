#!/usr/bin/env ruby

dir = File.dirname(__FILE__)
files = Dir.glob(File.join(dir, "test_*.rb")).sort

total_tests = 0
total_fails = 0

files.each do |f|
  name = File.basename(f, ".rb")
  # run in subprocess to isolate
  output = `ruby #{f} 2>&1`
  status = $?

  if status.success?
    if output =~ /All (\d+) tests passed/
      count = $1.to_i
      total_tests += count
      puts "  #{name}: #{count} tests OK"
    else
      puts "  #{name}: OK (unknown count)"
    end
  else
    lines = output.lines
    fails = lines.select { |l| l.start_with?("FAIL:") }
    if output =~ /(\d+) failures \/ (\d+) tests/
      total_fails += $1.to_i
      total_tests += $2.to_i
    end
    puts "  #{name}: FAILED"
    fails.each { |l| puts "    #{l}" }
  end
end

puts
if total_fails > 0
  puts "TOTAL: #{total_fails} failures / #{total_tests} tests"
  exit 1
else
  puts "TOTAL: All #{total_tests} tests passed"
end
