#!/usr/bin/env ruby
# Compare wastro across optimization tiers vs wasmtime on a few wasm
# programs.  Used by `make bench`.
#
# Tiers:
#   plain      — pure interpreter, no code-store
#   AOT 1st    — clear cache, gcc-compile all funcs, run (compile cost included)
#   AOT cached — code_store/all.so already on disk, just dlopen + run
#   wasmtime   — Cranelift JIT, --invoke
#
# Each measurement is the median of N runs (default 5).

require "open3"

WASTRO   = File.expand_path("./wastro", __dir__)
WASMTIME = ENV["WASMTIME"] || File.expand_path("~/.wasmtime/bin/wasmtime")
RUNS     = (ENV["BENCH_RUNS"] || 5).to_i

unless File.executable?(WASTRO)
  abort "#{WASTRO} not built — run 'make' first."
end
unless File.executable?(WASMTIME)
  abort "wasmtime not found at #{WASMTIME} (set WASMTIME=...)"
end

BENCHES = [
  ["fib(35)",         "examples/fib.wat",        "fib",          %w[35]],
  ["tak(22,14,6)",    "examples/tak.wat",        "tak",          %w[22 14 6]],
  ["ack(3,11)",       "examples/ack.wat",        "ack",          %w[3 11]],
  ["nqueens(11)",     "examples/nqueens.wat",    "queens",       %w[11]],
  ["fannkuch(10)",    "examples/fannkuch.wat",   "fannkuch",     %w[10]],
  ["sum_loop(1e7)",   "examples/sum_loop.wat",   "sum",          %w[10000000]],
  ["sieve(1e6)",      "examples/sieve.wat",      "count_primes", %w[1000000]],
  ["heapsort(50k)",   "examples/heapsort.wat",   "heapsort",     %w[50000]],
  ["pi(1e7)",         "examples/pi.wat",         "pi",           %w[10000000]],
  ["mandelbrot(300)", "examples/mandelbrot.wat", "mandel",       %w[300]],
  ["nbody(1e6)",      "examples/nbody.wat",      "nbody",        %w[1000000]],
  ["spectral(150)",   "examples/spectral.wat",   "spectral",     %w[150]],
  ["matmul(128)",     "examples/matmul.wat",     "matmul",       %w[128]],
  ["sha256(2000)",    "examples/sha256.wat",     "sha256_bench", %w[2000]],
].freeze

def time_ms(*cmd)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  Open3.capture3(*cmd)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  ((t1 - t0) * 1000).to_i
end

def median(xs)
  xs.sort[xs.size / 2]
end

def measure(label, runs:, fresh: false, &block)
  times = runs.times.map do
    FileUtils.rm_rf("code_store") if fresh
    block.call
  end
  median(times)
end

require "fileutils"

def bench_one(wat, fn, args)
  plain = measure("plain", runs: RUNS) {
    time_ms(WASTRO, "--no-compile", "-q", wat, fn, *args)
  }
  aot_first = measure("AOT 1st", runs: RUNS, fresh: true) {
    time_ms(WASTRO, "-c", "-q", wat, fn, *args)
  }
  # Pre-warm cache, then time dlopen-only runs.
  Open3.capture3(WASTRO, "--clear-cs", "-c", "-q", wat, fn, *args)
  aot_cached = measure("AOT cached", runs: RUNS) {
    time_ms(WASTRO, "-q", wat, fn, *args)
  }
  wasmtime = measure("wasmtime", runs: RUNS) {
    time_ms(WASMTIME, "run", "--invoke", fn, wat, *args)
  }
  [plain, aot_first, aot_cached, wasmtime]
end

# ---- streaming output with fixed column widths
name_w  = [BENCHES.map { |b| b[0].size }.max, "benchmark".size].max
num_w   = 8   # "99999 ms" fits
ratio_w = 9   # "1.23x win" fits

fmt = ->(name, *cells) {
  parts = [name.ljust(name_w)]
  cells.each_with_index do |c, i|
    w = (i == cells.size - 1) ? ratio_w : num_w
    parts << c.rjust(w)
  end
  parts.join("  ")
}

puts fmt.call("benchmark", "plain", "AOT-1st", "AOT-cached", "wasmtime", "vs.JIT")
puts "-" * (name_w + (num_w + 2) * 4 + ratio_w + 2)

results = []
BENCHES.each do |name, wat, fn, args|
  plain, aot1, aotc, wt = bench_one(wat, fn, args)
  ratio  = wt.zero? ? "-" : ("%.2fx" % (aotc.to_f / wt))
  marker = aotc < wt ? " win" : ""
  puts fmt.call(name,
                "#{plain} ms", "#{aot1} ms", "#{aotc} ms", "#{wt} ms",
                "#{ratio}#{marker}")
  $stdout.flush
  results << [name, plain, aot1, aotc, wt]
end

wins = results.count { |_, _, _, c, w| c < w }
puts
puts "wastro AOT cached beats wasmtime in #{wins} / #{results.size} benchmarks"
