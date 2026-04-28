#!/usr/bin/env ruby
# Microbenchmark: tight loop that updates N i32 locals per iteration.
# Generates a fresh .wat for each N, AOT-compiles via wastro, runs,
# and reports median wall time vs wasmtime.
#
# Background: every wasm local in wastro lives in a `union wastro_slot`
# slot in caller-allocated frame memory.  gcc can't keep them in
# registers across the dispatch chain, so each local roughly costs
# one cache-hit memory access per loop iteration.  This bench measures
# that per-local overhead and compares to wasmtime which puts wasm
# locals straight into CPU registers.
#
# Run from sample/wastro/.

require "open3"
require "fileutils"

WASTRO   = File.expand_path("./wastro", __dir__)
WASMTIME = File.expand_path("~/.wasmtime/bin/wasmtime")
ITERS    = 50_000_000

abort "build wastro first (make wastro)" unless File.executable?(WASTRO)

def gen_wat(n_locals)
  locals = (0...n_locals).map { |i| "(local $a#{i} i32)" }.join(" ")
  body = (0...n_locals).map { |i|
    "(local.set $a#{i} (i32.add (local.get $a#{i}) (local.get $i)))"
  }.join("\n      ")
  checksum = if n_locals.zero?
    "(i32.const 0)"
  else
    (0...n_locals).map { |i| "(local.get $a#{i})" }.reduce { |acc, x|
      "(i32.add #{acc} #{x})"
    }
  end
  <<~WAT
  (module
    (func $bench (export "bench") (param $iters i32) (result i32)
      (local $i i32) #{locals}
      (block $end (loop $L
        (br_if $end (i32.ge_s (local.get $i) (local.get $iters)))
        #{body}
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L)))
      #{checksum}))
  WAT
end

def time_wastro(wat_path)
  Open3.capture3(WASTRO, "--clear-cs", "-q", "-c", wat_path, "bench", ITERS.to_s)
  times = []
  3.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    Open3.capture3(WASTRO, "-q", wat_path, "bench", ITERS.to_s)
    times << ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000).to_i
  end
  times.sort[1]
end

def time_wasmtime(wat_path)
  times = []
  3.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    Open3.capture3(WASMTIME, "run", "--invoke", "bench", wat_path, ITERS.to_s)
    times << ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000).to_i
  end
  times.sort[1]
end

tmp = File.expand_path("examples/_local_bench.wat", __dir__)
ENV["CCACHE_DISABLE"] = "1"

puts "  N | wastro (ms) | wasmtime (ms)"
puts "----+-------------+--------------"
[0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 32].each do |n|
  File.write(tmp, gen_wat(n))
  printf("%3d | %11d | %12d\n", n, time_wastro(tmp), time_wasmtime(tmp))
end
File.delete(tmp)
