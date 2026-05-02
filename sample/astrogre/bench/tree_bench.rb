#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Recursive tree-walk bench — what `are` / `rg` / `grep -r`
# actually do day-to-day on a code tree.  Walker concerns dominate
# (.gitignore parsing, parallel directory walking, per-file
# open/read overhead) — not the regex engine itself.
#
# Usage: ruby tree_bench.rb [N]   (best-of-N, default 3)
# Env:   RAW=/path/to/results.tsv  → also dump per-run TSV
#
# Like grep_bench.rb, timed output goes to a regular file (not
# /dev/null) so GNU grep doesn't short-circuit via the dev_null
# detection added in 2016.

N    = (ARGV[0] || 3).to_i
RAW  = ENV["RAW"]
ARE  = ENV["ARE"]  || "../are/are"
GREP = "/usr/bin/grep"
OUT  = ENV["OUT"]  || "/tmp/tree_bench.out"

at_exit { File.unlink(OUT) rescue nil }

RG = [
  "/usr/bin/rg",
  "/usr/local/bin/rg",
  *Dir.glob("/home/ko1/.vscode-server/extensions/github.copilot-chat-*/" \
            "node_modules/@github/copilot/sdk/ripgrep/bin/linux-x64/rg"),
  *Dir.glob("/home/ko1/.vscode-server/cli/servers/Stable-*/server/" \
            "node_modules/@vscode/ripgrep/bin/rg"),
].find { |p| File.executable?(p) }

Dir.chdir(__dir__)
abort "build #{ARE} first (cd ../are && make)" unless File.executable?(ARE)

raw_io =
  if RAW
    f = File.open(RAW, "w")
    f.puts %w[tool label pattern run seconds].join("\t")
    f
  end
$raw_io = raw_io

def raw_row(tool, label, pattern, run, seconds)
  return unless $raw_io
  $raw_io.puts [tool, label, pattern, run, seconds].join("\t")
end

def best_of(tag, label, pattern, cmd_argv)
  best = Float::INFINITY
  ok_any = false
  N.times do |i|
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    pid = Process.spawn(*cmd_argv, out: OUT, err: "/dev/null")
    Process.wait(pid)
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
    rc = $?.exitstatus
    if rc.to_i.between?(0, 1)
      ok_any = true
      best = elapsed if elapsed < best
      raw_row(tag, label, pattern, i, "%.6f" % elapsed)
    else
      raw_row(tag, label, pattern, i, "ERR")
    end
  end
  if ok_any
    printf("  %-22s %.3f s\n", tag, best)
  else
    printf("  %-22s ERR\n", tag)
  end
end

# `pat` is a regex alternation.  `t_short` is the type filter name
# that both rg's `-t` and our `-t` understand (c, ruby, py, ...).
def run_tree(label, pattern, tree, t_short)
  unless File.directory?(tree)
    puts
    puts "[#{label}] (skip — no #{tree})"
    return
  end
  puts
  puts "[#{label}] /#{pattern}/  in #{tree}  (-t #{t_short})"
  best_of "are -j4",  label, pattern,
          [ARE, "-j", "4", "-c", "-t", t_short, "--", pattern, tree]
  best_of "ripgrep",  label, pattern,
          [RG, "-c", "-t", t_short, "--", pattern, tree] if RG

  # grep -r equivalents of `-t c` / `-t ruby` / `-t py`.
  incl = case t_short
         when "c"    then ["--include=*.c", "--include=*.h"]
         when "ruby" then ["--include=*.rb", "--include=Rakefile", "--include=Gemfile"]
         when "py"   then ["--include=*.py"]
         else             []
         end
  best_of "grep -r",  label, pattern,
          [GREP, "-rcE", *incl, "--", pattern, tree]
end

puts "best of #{N} runs · -j 4 for are · same -t LANG / --include for grep"
puts "raw: #{RAW}" if RAW

# Single-literal: walker dominates.
run_tree "1lit usr/include",   "CONFIG", "/usr/include", "c"

# Multi-literal alt that fits in byteset (≤ 8 first bytes).
run_tree "3lit byteset",
  "PROT_READ|PROT_WRITE|MAP_PRIVATE", "/usr/include", "c"

# Multi-literal alt that needs AC (> 8 distinct first bytes).
run_tree "12lit AC",
  "PROT_READ|PROT_WRITE|MAP_PRIVATE|MAP_SHARED|S_ISREG|S_ISDIR|EBADF|EAGAIN|EBUSY|EINTR|ENOMEM|EINVAL",
  "/usr/include", "c"

# Astro tree — small post-gitignore, exercises the .gitignore walker.
run_tree "astro tree verbose_mark", "verbose_mark", "../..", "c"

puts
