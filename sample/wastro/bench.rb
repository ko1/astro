#!/usr/bin/env ruby
# Compare wastro across optimization tiers vs wasmtime on a few wasm
# programs.  Used by `make bench`.
#
# Tiers:
#   plain         — pure interpreter, no code-store
#   AOT 1st       — clear cache, gcc-compile all funcs, run (compile cost included)
#   AOT cached    — code_store/all.so already on disk, just dlopen + run
#   wasmtime JIT  — Cranelift JIT, --invoke (compile + run)
#   wasmtime AOT  — `wasmtime compile` precompiled .cwasm, `--allow-precompiled`
#
# Each measurement is the median of N runs (default 5).

require "open3"

WASTRO   = File.expand_path("./wastro", __dir__)
WASMTIME = ENV["WASMTIME"] || File.expand_path("~/.wasmtime/bin/wasmtime")
RUNS     = (ENV["BENCH_RUNS"] || 5).to_i

# Disable ccache so the "AOT 1st" column reflects a true cold gcc
# compile.  Without this, ccache silently turns repeated SD compiles
# into ~10 ms hash lookups and makes the column meaningless.
ENV["CCACHE_DISABLE"] = "1"

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
  wasmtime_jit = measure("wasmtime JIT", runs: RUNS) {
    time_ms(WASMTIME, "run", "--invoke", fn, wat, *args)
  }
  # Precompile once, then time precompiled runs.  Cwasm is per-machine
  # / per-wasmtime-version so we regenerate each run; the compile is
  # outside the timed loop.
  cwasm = "_bench.cwasm"
  Open3.capture3(WASMTIME, "compile", wat, "-o", cwasm)
  wasmtime_aot = measure("wasmtime AOT", runs: RUNS) {
    time_ms(WASMTIME, "run", "--allow-precompiled", "--invoke", fn, cwasm, *args)
  }
  File.delete(cwasm) if File.exist?(cwasm)
  [plain, aot_first, aot_cached, wasmtime_jit, wasmtime_aot]
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

puts fmt.call("benchmark", "plain", "AOT-1st", "AOT-cached", "wt JIT", "wt AOT", "vs.AOT")
puts "-" * (name_w + (num_w + 2) * 5 + ratio_w + 2)

results = []
BENCHES.each do |name, wat, fn, args|
  plain, aot1, aotc, wt_jit, wt_aot = bench_one(wat, fn, args)
  # vs.AOT compares wastro AOT cached vs wasmtime AOT (the fairest pair —
  # both have compile cost amortized).
  ratio  = wt_aot.zero? ? "-" : ("%.2fx" % (aotc.to_f / wt_aot))
  marker = aotc < wt_aot ? " win" : ""
  puts fmt.call(name,
                "#{plain} ms", "#{aot1} ms", "#{aotc} ms",
                "#{wt_jit} ms", "#{wt_aot} ms",
                "#{ratio}#{marker}")
  $stdout.flush
  results << [name, plain, aot1, aotc, wt_jit, wt_aot]
end

wins = results.count { |r| r[3] < r[5] }   # AOT cached < wasmtime AOT
puts
puts "wastro AOT cached beats wasmtime AOT in #{wins} / #{results.size} benchmarks"
