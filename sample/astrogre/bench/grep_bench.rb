#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Whole-engine comparison rig.  ASTro framework's experimental
# result table — plain (interp) vs AOT-first (cold compile) vs
# AOT-cached (warm load) vs Onigmo backend, side by side with
# grep / ripgrep, on the same corpus and patterns.
#
# Usage: ruby grep_bench.rb [N]   (best-of-N, default N=3)
# Env:   RAW=/path/to/results.tsv  → also dump per-run TSV
#        ASTROGRE=/alt/binary, ARE=/alt/are/are
#
# Importantly, timed output goes to a regular file, NOT /dev/null.
# GNU grep since commit af6af288 (2016, "/dev/null output speedup")
# fstats stdout, detects /dev/null, and switches to first-match-
# and-exit mode internally — so any bench that pipes to /dev/null
# measures "first-match search" not "full scan".  Cost us hours of
# confusion before we caught it.

require "fileutils"
require "open3"

N        = (ARGV[0] || 3).to_i
RAW      = ENV["RAW"]
ASTROGRE = ENV["ASTROGRE"] || "../astrogre"
ARE      = ENV["ARE"]      || "../are/are"

# astro_cs creates `code_store/` relative to the launching process's
# cwd.  This script cd's into bench/, so the cache lands at
# bench/code_store/.  We blow it away before each AOT-first timed
# run to include the cs_compile + cs_build cost.
CODE_STORE = ENV["CODE_STORE"] || "code_store"

# Where to throw timed output.  Regular file (in tmpfs on most
# systems) avoids GNU grep's /dev/null short-circuit.
OUT = ENV["OUT"] || "/tmp/grep_bench.out"

at_exit { File.unlink(OUT) rescue nil }

# Locate /usr/bin/grep specifically — `grep` in our shells is
# often a Claude Code wrapper that secretly invokes ugrep with
# extra ignore-files flags, which would skew the comparison.
GREP = "/usr/bin/grep"

# Same wrapper concern for `rg`; pick a real binary.
RG = [
  "/usr/bin/rg",
  "/usr/local/bin/rg",
  *Dir.glob("/home/ko1/.vscode-server/extensions/github.copilot-chat-*/" \
            "node_modules/@github/copilot/sdk/ripgrep/bin/linux-x64/rg"),
  *Dir.glob("/home/ko1/.vscode-server/cli/servers/Stable-*/server/" \
            "node_modules/@vscode/ripgrep/bin/rg"),
].find { |p| File.executable?(p) }

# AOT cc invocation needs ccache off in sandboxed dirs.
ENV["CCACHE_DISABLE"] = "1"

# Working dir matches the old shell script (cd into bench/).
Dir.chdir(__dir__)

unless File.executable?(ASTROGRE)
  abort "build #{ASTROGRE} first (cd .. && make)"
end
puts "(no #{ARE} — skipping the are -j1 row)" unless File.executable?(ARE)

corpus =
  if    File.exist?("corpus_big.txt") then "corpus_big.txt"
  elsif File.exist?("corpus.txt")     then "corpus.txt"
  else  abort "no corpus — generate corpus.txt or corpus_big.txt"
  end
lines = File.foreach(corpus).count
bytes = File.size(corpus)

raw_io =
  if RAW
    f = File.open(RAW, "w")
    f.puts %w[tool label pattern run seconds].join("\t")
    f
  end

# Helper that runs `cmd_argv` N times, redirects stdout to OUT (so
# output cost is real but not /dev/null'd), and reports the best.
# `clear_cs` clears code_store/ before every iteration — used by the
# AOT-first variant to include compile cost in each measurement.
def best_of(tag, label, pattern, cmd_argv, clear_cs: false)
  best = Float::INFINITY
  ok_any = false
  N.times do |i|
    FileUtils.rm_rf(CODE_STORE) if clear_cs
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

def raw_row(tool, label, pattern, run, seconds)
  return unless $raw_io
  $raw_io.puts [tool, label, pattern, run, seconds].join("\t")
end
$raw_io = raw_io

# Run all the tools for one (label, pattern, extra-flags) tuple.
def run_pattern(label, pattern, *extra)
  puts
  puts "[#{label}] /#{pattern}/  #{extra.join(' ')}"

  # GNU grep, ripgrep
  best_of "grep -E",  label, pattern, [GREP, "-E", *extra, pattern, $corpus]
  best_of "ripgrep",  label, pattern, [$rg, "-j1", *extra, "-e", pattern, $corpus] if $rg

  # ASTro engine variants — the actual experiment.
  best_of "astrogre plain",       label, pattern,
          [$astrogre, "--plain", *extra, pattern, $corpus]
  best_of "astrogre +onigmo",     label, pattern,
          [$astrogre, "--backend=onigmo", *extra, pattern, $corpus]
  best_of "astrogre aot/first",   label, pattern,
          [$astrogre, "--aot-compile", *extra, pattern, $corpus],
          clear_cs: true
  # Warm the cache once before timing the cached path.
  Process.wait Process.spawn(
    $astrogre, "--aot-compile", *extra, pattern, $corpus,
    out: OUT, err: "/dev/null")
  best_of "astrogre aot/cached",  label, pattern,
          [$astrogre, "--aot-compile", *extra, pattern, $corpus]

  # are: production CLI on the same engine, defaults to interp.
  if $are
    best_of "are -j1", label, pattern,
            [$are, "-j", "1", *extra, "-e", pattern, $corpus]
  end
end

$astrogre = ASTROGRE
$are      = File.executable?(ARE) ? ARE : nil
$rg       = RG
$corpus   = corpus

puts "corpus: #{corpus}  (#{lines} lines, #{bytes} bytes)  best of #{N} runs"
puts "raw: #{RAW}" if RAW

run_pattern "literal",      "static"
run_pattern "literal-rare", "specialized_dispatcher"
run_pattern "anchored",     "^static"
run_pattern "case-insens",  "VALUE",                 "-i"
run_pattern "alt-3",        "static|extern|inline"
# alt-12: stresses the AC prefilter (>8 distinct first bytes).
run_pattern "alt-12",       "static|extern|inline|return|while|switch|break|case|goto|asm|defined|sizeof"
run_pattern "class-rep",    '[0-9]{4,}'
run_pattern "ident-call",   '[a-z_]+_[a-z]+\('
run_pattern "count",        "static",                "-c"
puts
