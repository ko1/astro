#!/usr/bin/env ruby
# Benchmark harness: runs each benchmark in three astr modes
# (interp / AOT-fresh / AOT-cached) plus GNU R when it's on PATH.
# Reports min wall-clock per mode after a single warm-up.

require 'benchmark'

ROOT = File.expand_path('..', __dir__)
BIN  = File.join(ROOT, 'astr')
abort "binary missing: #{BIN} (run `make`)" unless File.executable?(BIN)

WARMUP = 1
ITERS  = 3

def time(cmd)
  ts = []
  (WARMUP + ITERS).times do |i|
    t = Benchmark.realtime { system(*cmd, out: File::NULL, err: File::NULL) }
    ts << t if i >= WARMUP
  end
  ts.min
end

R_BIN = %w[Rscript /usr/bin/Rscript /usr/local/bin/Rscript].find { File.executable?(_1) }

benches = ARGV.empty? ? Dir.glob(File.join(__dir__, '*.r')).sort : ARGV.map { File.expand_path(_1) }

modes = []
modes << ['astr-interp',     ->(p) { [BIN, '-i', '-q', p] }]
modes << ['astr-aot-fresh',  ->(p) {
  # bake into a fresh code_store each iteration so we measure the cost
  # of bake + run together — what `make c` shows on a clean checkout.
  system('rm', '-rf', File.join(ROOT, 'code_store'))
  [BIN, '-c', '-q', p]
}]
modes << ['astr-aot-cached', ->(p) {
  # warm up the code_store once before timing so the timed run only
  # pays the cs_load + dispatch cost.
  system(BIN, '-c', '-q', p, out: File::NULL, err: File::NULL)
  [BIN, '-q', p]
}]
if R_BIN
  modes << ['R-base',        ->(p) { [R_BIN, '--vanilla', '-e', "source('#{p}')"] }]
else
  warn "(R not found — skipping R-base comparison)"
end

puts format('%-22s  %-18s  %s', 'bench', 'mode', 'min wall (s)')
benches.each do |path|
  name = File.basename(path, '.r')
  modes.each do |label, cmd|
    t = time(cmd.call(path))
    puts format('%-22s  %-18s  %.3f', name, label, t)
  end
end
