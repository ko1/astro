#!/usr/bin/env ruby
# Optcarrot benchmark runner — outputs a Markdown table.
#
# Usage: ruby benchmark/optcarrot_bench.rb
#        make bench-optcarrot-all

OPTCARROT = "benchmark/optcarrot/bin/optcarrot-bench"
EXTRA_ARGS = "--print-video-checksum"
EXPECTED_CHECKSUM = 59662

RUNNERS = [
  ["ruby",                "ruby #{OPTCARROT} #{EXTRA_ARGS}"],
  ["ruby --jit",          "ruby --jit #{OPTCARROT} #{EXTRA_ARGS}"],
  ["abruby --plain",      "exe/abruby --plain #{OPTCARROT} #{EXTRA_ARGS}"],
  ["abruby -c",           -> { system("rm -rf code_store") # not counted in timing
                               "CCACHE_DISABLE=1 exe/abruby -c #{OPTCARROT} #{EXTRA_ARGS}" }],
  ["abruby --compiled-only", "exe/abruby --compiled-only #{OPTCARROT} #{EXTRA_ARGS}"],
]

results = []

RUNNERS.each do |label, cmd|
  if cmd.is_a?(Proc)
    cmd = cmd.call
  end

  $stderr.print "  #{label}... "
  $stderr.flush

  start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  output = `#{cmd} 2>&1`
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start

  fps = output[/fps: ([0-9.]+)/, 1]
  checksum = output[/checksum: (\d+)/, 1]

  unless checksum == EXPECTED_CHECKSUM.to_s
    $stderr.puts "CHECKSUM MISMATCH: expected #{EXPECTED_CHECKSUM}, got #{checksum.inspect}"
    $stderr.puts output
    exit 1
  end

  time_s = "%.2fs" % elapsed
  fps_f = "%.2f" % fps.to_f

  $stderr.puts "#{fps_f} fps  #{time_s}"
  results << [label, fps_f, time_s]
end

puts
puts "| runner | fps | time |"
puts "|--------|----:|-----:|"
results.each do |label, fps, time|
  puts "| #{label} | #{fps} | #{time} |"
end
