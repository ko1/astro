#!/usr/bin/env ruby

dir = File.dirname(__FILE__)
files = Dir.glob(File.join(dir, "test_*.rb")).sort
failed = false
total_runs = 0
total_failures = 0
total_errors = 0

files.each do |f|
  output = `ruby #{f} 2>&1`
  name = File.basename(f, ".rb")
  if $?.success?
    if output =~ /(\d+) runs, (\d+) assertions, (\d+) failures, (\d+) errors/
      runs, _asrt, fail, err = $1.to_i, $2.to_i, $3.to_i, $4.to_i
      total_runs += runs
      total_failures += fail
      total_errors += err
      status = (fail + err == 0) ? "OK" : "FAIL"
      puts "  [#{status}] #{name}: #{runs} runs, #{fail} failures, #{err} errors"
      failed = true if fail + err > 0
    else
      puts "  [????] #{name}: no minitest summary"
      puts output.lines.last(5).map { |l| "    #{l}" }.join
      failed = true
    end
  else
    puts "  [CRASH] #{name}"
    puts output.lines.first(20).map { |l| "    #{l}" }.join
    failed = true
  end
end

puts "---"
puts "TOTAL: #{total_runs} runs, #{total_failures} failures, #{total_errors} errors"
exit(failed ? 1 : 0)
