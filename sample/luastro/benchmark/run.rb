#!/usr/bin/env ruby
# luastro benchmark runner.  Compares interpreter / AOT / lua5.4 / luajit.
#
# Engines (mirrors abruby's benchmark/run.rb conventions):
#
#   luastro          — pure interpreter, no code store consulted.
#   luastro-AOT-1st  — `-c`: bake SDs and run in one process.  Time
#                      includes gcc compilation of code_store/all.so;
#                      cleared before every iteration so each run is a
#                      cold start.  CCACHE_DISABLE=1 keeps gcc honest.
#   luastro-AOT-c    — cached AOT: bake once in `setup` (not timed),
#                      then time `./luastro file.lua` runs that load
#                      the warmed code_store/all.so.
#   lua5.4 / luajit  — reference implementations.
#
# Usage:
#   ruby benchmark/run.rb              # all benchmarks
#   ruby benchmark/run.rb fib tak      # subset (substring match)
#   N=3 ruby benchmark/run.rb          # take best of N timed runs

require 'open3'

DIR    = File.dirname(__FILE__)
PARENT = File.expand_path("..", DIR)
EXE    = File.expand_path("../luastro", DIR)
RUNS   = (ENV["N"] || "1").to_i

abort "luastro not built" unless File.executable?(EXE)

args = ARGV
files = Dir["#{DIR}/bm_*.lua"].sort
files = files.select { |f| args.any? { |a| File.basename(f).include?(a) } } unless args.empty?

# Each engine is a hash:
#   name:  display label
#   prep:  shell command run before EVERY timed iteration (not timed)
#   setup: shell command run ONCE before timing begins (not timed)
#   cmd:   command template — '%s' is replaced with the .lua filename
ENGINES = [
  {
    name: "luastro",
    cmd:  "#{EXE} %s",
  },
  {
    # AOT cold-start: compile + run in one process every iteration.
    # CCACHE_DISABLE=1 forces gcc to compile from scratch (no ccache).
    name: "luastro-AOT-1st",
    prep: "rm -rf #{PARENT}/code_store",
    cmd:  "CCACHE_DISABLE=1 #{EXE} -c %s",
  },
  {
    # AOT steady-state: bake once, then time pure execution against the
    # warmed code_store/all.so.
    name:  "luastro-AOT-c",
    setup: "rm -rf #{PARENT}/code_store && CCACHE_DISABLE=1 #{EXE} --aot-compile %s",
    cmd:   "#{EXE} %s",
  },
  {
    name: "lua5.4",
    cmd:  "lua5.4 %s",
  },
  {
    name: "luajit",
    cmd:  "luajit %s",
  },
]

def installed?(label)
  case label
  when "lua5.4" then system("which lua5.4 >/dev/null 2>&1")
  when "luajit" then system("which luajit >/dev/null 2>&1")
  else true
  end
end

engines = ENGINES.select { |e| installed?(e[:name]) }

# --- table header ------------------------------------------------------

header = "%-22s" % "benchmark"
engines.each { |e| header += " | %-15s" % e[:name] }
puts header
puts "-" * header.length

def run_cmd_timed(cmd_str)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out, err, st = Open3.capture3({"CCACHE_DISABLE" => "1"}, "sh", "-c", cmd_str)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  [t1 - t0, st, out, err]
end

def run_cmd_silent(cmd_str)
  system({"CCACHE_DISABLE" => "1"}, "sh", "-c", cmd_str,
         out: File::NULL, err: File::NULL)
end

# --- per-benchmark runner ----------------------------------------------

files.each do |f|
  base = File.basename(f, ".lua").sub(/^bm_/, "")
  row = "%-22s" % base
  engines.each do |e|
    # One-time setup, not timed.
    if e[:setup]
      cmd = e[:setup].gsub("%s", f)
      ok = run_cmd_silent(cmd)
      unless ok
        row += " | %-15s" % "SETUP_ERR"
        next
      end
    end

    times = []
    last_st = nil
    RUNS.times do
      # Per-iteration prep, not timed.
      if e[:prep]
        run_cmd_silent(e[:prep].gsub("%s", f))
      end
      cmd = e[:cmd].gsub("%s", f)
      t, st, _out, _err = run_cmd_timed(cmd)
      last_st = st
      if !st.success?
        times = [Float::NAN]
        break
      end
      times << t
    end
    best = times.compact.min
    cell = if best.respond_to?(:nan?) && best.nan?
             "ERR"
           else
             "%9.3fs" % best
           end
    row += " | %-15s" % cell
  end
  puts row
end
