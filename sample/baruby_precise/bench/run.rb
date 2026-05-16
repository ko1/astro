#!/usr/bin/env ruby
# Benchmark runner for baruby's GC testbed.
#
# Runs each .ba.rb under bench/ and reports wall time + GC stats.  GC
# stats come from BARUBY_GC_STATS=1, which makes baruby print Boehm GC
# counters (heap size, total bytes alloc'd, collection count) on exit.

require 'optparse'

BENCH_DIR = File.expand_path("..", __FILE__)
BARUBY    = ENV["BARUBY_BIN"] || File.expand_path("../baruby_precise", BENCH_DIR)

opts = { mode: "plain", repeats: 3 }
OptionParser.new do |o|
  o.on("--mode MODE", "plain | aot | pg (default plain)") { opts[:mode] = _1 }
  o.on("-n N", Integer, "repeats per bench (default 3)")  { opts[:repeats] = _1 }
end.parse!(ARGV)

flag = case opts[:mode]
       when "plain" then ["--plain"]
       when "aot"   then ["-c"]
       when "pg"    then ["-p"]
       else abort "unknown mode #{opts[:mode]}"
       end

benches = ARGV.empty? ? Dir["#{BENCH_DIR}/*.ba.rb"].sort : ARGV
puts "mode: #{opts[:mode]}, repeats: #{opts[:repeats]}"
puts "%-24s %10s %10s %10s %10s %10s %8s" %
     ["bench", "best(s)", "med(s)", "alloc_MB", "GCs", "gc_s", "gc%"]

benches.each do |path|
  name = File.basename(path, ".ba.rb")
  times = []
  alloc_mb = nil
  gcs      = nil
  gc_sec   = nil
  gc_pct   = nil
  opts[:repeats].times do
    env = { "BARUBY_GC_STATS" => "1" }
    cmd = [env, BARUBY, *flag, path]
    out = IO.popen(cmd, err: [:child, :out]) { |io| io.read }
    real = out[/real\s+(\S+)/, 1]&.to_f
    real ||= out[/__ELAPSED__\s+(\S+)/, 1]&.to_f
    times << real if real
    if (m = out.match(/__GC_STATS__\b.*?alloc_bytes=(\d+)\b.*?gc_count=(\d+)/))
      alloc_mb = m[1].to_i / (1024.0 * 1024.0)
      gcs      = m[2].to_i
    end
    if (m = out.match(/gc_seconds=([\d.]+)\s+gc_pct=([\d.]+)/))
      gc_sec = m[1].to_f
      gc_pct = m[2].to_f
    end
  end
  if times.empty?
    puts "%-24s  no output" % name
  else
    sorted = times.sort
    puts "%-24s %10.3f %10.3f %10.1f %10d %10.3f %7.1f%%" %
         [name, sorted.first, sorted[sorted.size / 2],
          alloc_mb || 0.0, gcs || 0, gc_sec || 0.0, gc_pct || 0.0]
  end
end
