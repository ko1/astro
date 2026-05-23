#!/usr/bin/env ruby
# Benchmark runner for baruby_precise's GC testbed.
#
# Per-bench:
#   - Run N repeats (default 3).  Each run captures its own time + GC stats.
#   - Stats are kept per-run so the chosen run (best/median/p10) reports its
#     own GC counters — not stats randomly carried over from a later iter
#     (iter 35 fairness fix).
#
# GC stats come from BARUBY_GC_STATS=1 — see main.c for the format.

require 'optparse'

BENCH_DIR = File.expand_path("..", __FILE__)
BARUBY    = ENV["BARUBY_BIN"] || File.expand_path("../baruby_precise", BENCH_DIR)

opts = { mode: "plain", repeats: 3, choose: "median" }
OptionParser.new do |o|
  o.on("--mode MODE",     "plain | aot | pg (default plain)") { opts[:mode] = _1 }
  o.on("-n N", Integer,   "repeats per bench (default 3)")    { opts[:repeats] = _1 }
  o.on("--choose POL",    "best | median | trimmed (default median)") { opts[:choose] = _1 }
end.parse!(ARGV)

flag = case opts[:mode]
       when "plain" then ["--plain"]
       when "aot"   then ["--aot-compile", "--run"]
       when "pg"    then ["--pg-compile"]
       else abort "unknown mode #{opts[:mode]}"
       end

# Parse a single run's output into a Hash with time + GC stats.
# Returns nil if no __ELAPSED__ found.
def parse_run(out)
  elapsed = out[/__ELAPSED__\s+(\S+)/, 1]&.to_f
  return nil unless elapsed
  h = { elapsed: elapsed }
  # __GC_STATS__ key=val pairs, space-separated.
  if (line = out[/^__GC_STATS__\s+(.*)$/, 1])
    line.split(/\s+/).each do |kv|
      k, v = kv.split('=', 2)
      h[k.to_sym] = v if k && v
    end
  end
  h
end

# Pick one run from `runs` (an array of parse_run hashes) per policy.
def pick(runs, policy)
  return nil if runs.empty?
  by_time = runs.sort_by { |r| r[:elapsed] }
  case policy
  when "best"     then by_time.first
  when "median"   then by_time[runs.size / 2]
  when "trimmed"  # drop fastest + slowest, average remainder; pick the median-equivalent
    return by_time[runs.size / 2] if runs.size < 4
    by_time[1..-2][by_time[1..-2].size / 2]
  else by_time[runs.size / 2]
  end
end

benches = ARGV.empty? ? Dir["#{BENCH_DIR}/*.ba.rb"].sort : ARGV
puts "mode: #{opts[:mode]}, repeats: #{opts[:repeats]}, choose: #{opts[:choose]}"
puts "%-24s %10s %10s %10s %10s %10s %10s %8s %10s" %
     ["bench", "wall(s)", "best(s)", "alloc_MB", "GCs", "gc_s", "mark_s", "gc%", "max_ms"]

benches.each do |path|
  name = File.basename(path, ".ba.rb")
  runs = []
  opts[:repeats].times do
    env = { "BARUBY_GC_STATS" => "1" }
    cmd = [env, BARUBY, *flag, path]
    out = IO.popen(cmd, err: [:child, :out]) { |io| io.read }
    h = parse_run(out)
    runs << h if h
  end
  if runs.empty?
    puts "%-24s  no output" % name
    next
  end
  chosen = pick(runs, opts[:choose])
  best   = runs.min_by { |r| r[:elapsed] }
  alloc_mb = (chosen[:alloc_bytes] || "0").to_i / (1024.0 * 1024.0)
  gcs      = (chosen[:gc_count]    || "0").to_i
  gc_sec   = (chosen[:gc_seconds]  || "0").to_f
  mark_sec = (chosen[:mark_seconds]|| "0").to_f
  gc_pct   = (chosen[:gc_pct]      || "0").to_f
  max_ms   = (chosen[:max_pause_ms]|| "0").to_f
  puts "%-24s %10.3f %10.3f %10.1f %10d %10.3f %10.3f %7.1f%% %10.2f" %
       [name, chosen[:elapsed], best[:elapsed],
        alloc_mb, gcs, gc_sec, mark_sec, gc_pct, max_ms]
end
