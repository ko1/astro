#!/usr/bin/env ruby

dir = File.dirname(__FILE__)
files = Dir.glob(File.join(dir, "test_*.rb")).sort
failed = false

files.each do |f|
  output = `ruby #{f} 2>&1`
  if $?.success?
    # extract minitest summary line
    if output =~ /(\d+) runs, (\d+) assertions, (\d+) failures, (\d+) errors/
      name = File.basename(f, ".rb")
      puts "  #{name}: #{$1} runs, #{$3} failures, #{$4} errors"
      failed = true if $3.to_i > 0 || $4.to_i > 0
    end
  else
    name = File.basename(f, ".rb")
    puts "  #{name}: CRASHED"
    puts output.lines.first(5).map { |l| "    #{l}" }.join
    failed = true
  end
end

exit(failed ? 1 : 0)
