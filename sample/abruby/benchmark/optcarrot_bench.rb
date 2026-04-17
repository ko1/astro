#!/usr/bin/env ruby
# Optcarrot benchmark runner — outputs a Markdown table.
#
# Usage: ruby benchmark/optcarrot_bench.rb
#        make bench-optcarrot-all

OPTCARROT = "benchmark/optcarrot/bin/optcarrot-bench"
EXTRA_ARGS = "--print-video-checksum"
EXPECTED_CHECKSUM = 59662

# Each runner: { label, setup, cmd, repeat }.
#   setup:  runs once before timing (optional, not timed)
#   cmd:    timed command
#   repeat: iteration count; reported time/fps is best-of-N.  Bake rows
#           are slow (compile cost) so we leave them at 1; cached /
#           steady-state rows default to 3 for noise immunity.
DEFAULT_REPEAT = 3

RUNNERS = [
  { label: "ruby",
    cmd:   "ruby #{OPTCARROT} #{EXTRA_ARGS}" },
  { label: "ruby --jit",
    cmd:   "ruby --jit #{OPTCARROT} #{EXTRA_ARGS}" },
  { label: "abruby --plain",
    cmd:   "exe/abruby --plain #{OPTCARROT} #{EXTRA_ARGS}" },

  # AOT: -c compiles + runs.  time includes every SD_<Horg>.c / all.so
  # build cost (intentional — this is the "from scratch" cost).  Slow,
  # 1-shot.
  { label:  "abruby -c (AOT bake)",
    setup:  "rm -rf code_store",
    cmd:    "CCACHE_DISABLE=1 exe/abruby -c #{OPTCARROT} #{EXTRA_ARGS}",
    repeat: 1 },
  { label: "abruby AOT (cached)",
    cmd:   "exe/abruby --compiled-only --aot-only #{OPTCARROT} #{EXTRA_ARGS}" },

  # PGC first-run: --pgc runs the interpreter (profile collection), then
  # at eval end bakes SD_<Hopt>.c + hopt_index.txt + all.so.  time reports
  # "interpreter execution + bake overhead".  Slow, 1-shot.
  { label:  "abruby --pgc (bake)",
    setup:  "rm -rf code_store",
    cmd:    "CCACHE_DISABLE=1 exe/abruby --pgc #{OPTCARROT} #{EXTRA_ARGS}",
    repeat: 1 },
  # PGC steady-state: --compiled-only prefers SD_<Hopt>, falling back to
  # SD_<Horg> for entries whose profile didn't diverge.
  { label: "abruby PGC (cached)",
    cmd:   "exe/abruby --compiled-only #{OPTCARROT} #{EXTRA_ARGS}" },
]

results = []

RUNNERS.each do |runner|
  label  = runner[:label]
  cmd    = runner[:cmd]
  repeat = runner[:repeat] || DEFAULT_REPEAT

  $stderr.print "  #{label}... "
  $stderr.flush

  # Setup (rm code_store etc.) is NOT part of the measurement.
  if runner[:setup]
    system(runner[:setup], out: File::NULL, err: File::NULL) \
      or abort("setup failed: #{runner[:setup]}")
  end

  iters = repeat.times.map {
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
    { elapsed: elapsed, fps: fps.to_f }
  }

  # Report best of N: lowest wall-clock + highest fps (they track each
  # other when the script prints one value, but pick them independently
  # to be safe).
  best = iters.min_by { |r| r[:elapsed] }
  best_fps = iters.map { |r| r[:fps] }.max
  time_s = "%.2fs" % best[:elapsed]
  fps_f  = "%.2f" % best_fps

  tag = repeat > 1 ? "best of #{repeat}" : "1 run"
  $stderr.puts "#{fps_f} fps  #{time_s}  (#{tag})"
  results << [label, fps_f, time_s]
end

puts
puts "| runner | fps | time |"
puts "|--------|----:|-----:|"
results.each do |label, fps, time|
  puts "| #{label} | #{fps} | #{time} |"
end
