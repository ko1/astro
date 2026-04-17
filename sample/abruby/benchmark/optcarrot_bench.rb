#!/usr/bin/env ruby
# Optcarrot benchmark runner — outputs a Markdown table.
#
# Usage: ruby benchmark/optcarrot_bench.rb
#        make bench-optcarrot-all

OPTCARROT = "benchmark/optcarrot/bin/optcarrot-bench"
EXTRA_ARGS = "--print-video-checksum"
EXPECTED_CHECKSUM = 59662

# Each runner: { label, setup (run before timing, optional), cmd (timed) }.
# For "(bake)" rows the compile cost intentionally lands in `cmd` so the
# user sees the one-shot cost.  For "(cached)" rows `cmd` reuses the
# store populated by the row above, measuring steady-state execution.
RUNNERS = [
  { label: "ruby",
    cmd:   "ruby #{OPTCARROT} #{EXTRA_ARGS}" },
  { label: "ruby --jit",
    cmd:   "ruby --jit #{OPTCARROT} #{EXTRA_ARGS}" },
  { label: "abruby --plain",
    cmd:   "exe/abruby --plain #{OPTCARROT} #{EXTRA_ARGS}" },

  # AOT: -c compiles + runs.  time includes every SD_<Horg>.c / all.so
  # build cost (intentional — this is the "from scratch" cost).
  { label: "abruby -c (AOT bake)",
    setup: "rm -rf code_store",
    cmd:   "CCACHE_DISABLE=1 exe/abruby -c #{OPTCARROT} #{EXTRA_ARGS}" },
  { label: "abruby AOT (cached)",
    cmd:   "exe/abruby --compiled-only --aot-only #{OPTCARROT} #{EXTRA_ARGS}" },

  # PGC first-run: --pgc runs the interpreter (profile collection), then
  # at eval end bakes SD_<Hopt>.c + hopt_index.txt + all.so.  time reports
  # "interpreter execution + bake overhead".
  { label: "abruby --pgc (bake)",
    setup: "rm -rf code_store",
    cmd:   "CCACHE_DISABLE=1 exe/abruby --pgc #{OPTCARROT} #{EXTRA_ARGS}" },
  # PGC steady-state: --compiled-only prefers SD_<Hopt>, falling back to
  # SD_<Horg> for entries whose profile didn't diverge.
  { label: "abruby PGC (cached)",
    cmd:   "exe/abruby --compiled-only #{OPTCARROT} #{EXTRA_ARGS}" },
]

results = []

RUNNERS.each do |runner|
  label = runner[:label]
  cmd   = runner[:cmd]

  $stderr.print "  #{label}... "
  $stderr.flush

  # Setup (rm code_store etc.) is NOT part of the measurement.
  if runner[:setup]
    system(runner[:setup], out: File::NULL, err: File::NULL) \
      or abort("setup failed: #{runner[:setup]}")
  end

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
