#!/usr/bin/env ruby
# frozen_string_literal: true
#
# AOT-favourable bench harness — patterns where the engine spends
# real time inside the AST chain (long alternations, multi-class
# repeats, capture-heavy regex), so dispatcher elimination is
# worth measuring.
#
# Astrogre is timed via `--bench-file` (whole 118 MB corpus loaded
# once, astrogre_search called N times in-process) so the engine
# cost is isolated from CLI per-line CTX-init overhead.  Other
# tools run via their CLI in count mode (`-c`) — they sweep the
# whole file in one go, which matches what `--bench-file` does.
#
# Usage: ruby aot_bench.rb [N]   (best-of-N, default 3)
# Env:   RAW=path/to/results.tsv
#        ASTROGRE=/alt/binary
#
# Same /dev/null pitfall as grep_bench.rb — GNU grep's
# af6af288 short-circuits on /dev/null output, so timed runs go to
# a regular file in tmpfs.

N        = (ARGV[0] || 3).to_i
RAW      = ENV["RAW"]
ASTROGRE = ENV["ASTROGRE"] || "../astrogre"
GREP     = "/usr/bin/grep"
OUT      = ENV["OUT"] || "/tmp/aot_bench.out"

at_exit { File.unlink(OUT) rescue nil }

RG = [
  "/usr/bin/rg",
  "/usr/local/bin/rg",
  *Dir.glob("/home/ko1/.vscode-server/extensions/github.copilot-chat-*/" \
            "node_modules/@github/copilot/sdk/ripgrep/bin/linux-x64/rg"),
  *Dir.glob("/home/ko1/.vscode-server/cli/servers/Stable-*/server/" \
            "node_modules/@vscode/ripgrep/bin/rg"),
].find { |p| File.executable?(p) }

ENV["CCACHE_DISABLE"] = "1"
Dir.chdir(__dir__)
abort "build #{ASTROGRE} first" unless File.executable?(ASTROGRE)

CORPUS = ENV["CORPUS"] ||
         (File.exist?("corpus_big.txt") ? "corpus_big.txt" : "corpus.txt")
abort "missing #{CORPUS}" unless File.exist?(CORPUS)
LINES = File.foreach(CORPUS).count
BYTES = File.size(CORPUS)

raw_io =
  if RAW
    f = File.open(RAW, "w")
    f.puts %w[tool label pattern run ms].join("\t")
    f
  end
$raw_io = raw_io
def raw_row(*c) = $raw_io&.puts(c.join("\t"))

# Pull `per=N.NNNms` out of `--bench-file` output.
def extract_per(text)
  m = text.match(/per=([0-9.]+)ms/)
  m ? m[1].to_f : nil
end

# In-engine bench: --bench-file runs the full sweep N times.
def astrogre_bench_file(pat_literal, mode)
  FileUtils.rm_rf("code_store") if mode == "--aot"
  out, _ = Open3.capture2e(
    ASTROGRE, "--bench-file", CORPUS, pat_literal, N.to_s, mode)
  extract_per(out)
end

# CLI bench (best of N) — for grep/ripgrep/onigmo backends.
def best_of_ms(tag, label, pattern, cmd_argv)
  best = nil
  N.times do |i|
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    pid = Process.spawn(*cmd_argv, out: OUT, err: "/dev/null")
    Process.wait(pid)
    elapsed_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000.0
    rc = $?.exitstatus
    if rc.to_i.between?(0, 1)
      best = elapsed_ms if best.nil? || elapsed_ms < best
      raw_row(tag, label, pattern, i, "%.3f" % elapsed_ms)
    else
      raw_row(tag, label, pattern, i, "ERR")
    end
  end
  best
end

require "fileutils"
require "open3"

# Pattern table.  `pat_literal` is the /.../-wrapped form astrogre
# wants from --bench-file; `pat_regex` is the bare form for the CLI
# tools' -E flag.
PATTERNS = [
  ["/(QQQ|RRR)+\\d+/",                          "(QQQ|RRR)+\\d+"],
  ["/(QQQX|RRRX|SSSX)+/",                       "(QQQX|RRRX|SSSX)+"],
  ["/[a-z]\\d[A-Z]\\d[a-z]\\d[A-Z]\\d[a-z]/",   "[a-z]\\d[A-Z]\\d[a-z]\\d[A-Z]\\d[a-z]"],
  ["/[A-Z]{50,}/",                              "[A-Z]{50,}"],
  ["/\\b(if|else|for|while|return)\\b/",        "\\b(if|else|for|while|return)\\b"],
  ["/[a-z][0-9][a-z][0-9][a-z]/",               "[a-z][0-9][a-z][0-9][a-z]"],
  ["/(\\d+\\.\\d+\\.\\d+\\.\\d+)/",             "(\\d+\\.\\d+\\.\\d+\\.\\d+)"],
  ["/(\\w+)\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\)/", "(\\w+)\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\)"],
]

puts "corpus: #{CORPUS}  (#{LINES} lines, #{BYTES} bytes)  best of #{N}"
puts "raw: #{RAW}" if RAW
printf "%-50s %8s %8s %9s %8s %8s\n",
       "pattern", "interp", "+aot", "+onigmo", "grep -E", "rg"
puts "-" * 100

PATTERNS.each do |pat_lit, pat_re|
  interp = astrogre_bench_file(pat_lit, "--plain")
  aot    = astrogre_bench_file(pat_lit, "--aot")
  ognm   = best_of_ms("astrogre +onigmo", pat_lit, pat_re,
                     [ASTROGRE, "--backend=onigmo", "-c", pat_re, CORPUS])
  grep_t = best_of_ms("grep -E", pat_lit, pat_re,
                     [GREP, "-E", "-c", pat_re, CORPUS])
  rg_t   = RG ? best_of_ms("ripgrep", pat_lit, pat_re,
                            [RG, "-j1", "-c", "-e", pat_re, CORPUS]) : nil

  raw_row("astrogre interp", pat_lit, pat_re, "best", "%.3f" % interp) if interp
  raw_row("astrogre aot",    pat_lit, pat_re, "best", "%.3f" % aot)    if aot

  fmt = ->(v) { v.nil? ? "ERR" : "%.3f" % v }
  printf "%-50s %8s %8s %9s %8s %8s\n",
         pat_lit, fmt.(interp), fmt.(aot), fmt.(ognm), fmt.(grep_t), fmt.(rg_t)
end
puts
