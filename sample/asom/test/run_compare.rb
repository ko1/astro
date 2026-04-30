#!/usr/bin/env ruby
# Side-by-side bench comparison: asom (interp/aot/pg) vs other SOM impls.
#
# Each engine is run once per benchmark via /usr/bin/time so we capture
# both metrics in a single trial:
#
#   inner-work  - the engine's own `system ticks` measurement of the
#                 benchmark loop. Excludes process startup / parse /
#                 JVM bootstrap / eager JIT — fair across implementations
#                 whose startup costs differ wildly.
#                   "<Bench>: iterations=N runtime: Yus"            (asom)
#                   "<Bench>: iterations=N average: Xus total: Yus" (SOM-st)
#
#   wall-clock  - `/usr/bin/time -f '%e'` over the entire process. The
#                 "user-experience" number; biased against JVM-hosted
#                 engines that pay 1.5-2 s of bootstrap.
#
# Two tables are printed at the end so the inner-work and wall numbers
# for the same benchmark sit side by side. Each cell is best-of-3,
# picked independently per metric from the same three trials.
#
# Engines:
#   asom/interp : --plain (no code store)
#   asom/aot    : -c --preload=<bench>  (bake before run)
#   asom/pg     : -p --preload=<bench>  (post-run hot bake; second run uses)
#   SOM++       : SOM-st/SOMpp, USE_TAGGING + COPYING + g++ -O3 -flto
#   Truffle     : SOM-st/TruffleSOM on GraalVM CE 25 with libgraal
#
# CSOM and plain-CPython PySOM are omitted on purpose — pedagogical /
# untranslated implementations, not a perf reference. To compare against
# PySOM properly, translate it via RPython (`make som-ast-jit`) and add
# a column for the resulting JIT binary.
#
# Usage:
#   ruby run_compare.rb [ITERS]    # default 5
#
# Each trial runs <ITERS> outer iterations within a single engine
# process. parse_inner drops the first half of those and averages the
# rest — so JIT-bearing engines (Truffle) reach warm peak before we
# start sampling. ITERS=5 → drop 2, average 3 (covers Truffle's
# typical 2-3 outer-iter warmup on AWFY).

require 'fileutils'
require 'tempfile'

ITERS = (ARGV[0] || '5').to_i

ASOM_DIR        = File.expand_path('..', __dir__)
SOMPP_DIR       = ENV.fetch('SOMPP_DIR',      "#{ASOM_DIR}/SOMpp")
TRUFFLESOM_DIR  = ENV.fetch('TRUFFLESOM_DIR', "#{ASOM_DIR}/TruffleSOM")
MX_DIR          = ENV.fetch('MX_DIR',         "#{ASOM_DIR}/mx")

BENCHES = %w[Sieve Permute Towers Queens List Storage
             Bounce BubbleSort QuickSort TreeSort
             Fannkuch Mandelbrot]

# Per-bench inner-iteration count. Sized so asom-interp runs ~1 s per
# bench (so AOT/PG/SOM++ land around 0.2-0.7 s and Truffle is sub-ms,
# all comfortably out of setup-bound territory). Short runs (ms scale)
# are setup-bound — IC prime, GC space init, swap_dispatcher cold path
# — and don't reflect steady-state throughput. Values were calibrated
# from the per-iteration cost in an earlier `make bench` run on this
# machine; tune up/down by ~2x if the harness runs on a faster/slower
# host.
INNER = {
  'Sieve'      => 1200,
  'Permute'    => 300,
  'Towers'     => 120,
  'Queens'     => 500,
  'List'       => 320,
  'Storage'    => 400,
  'Bounce'     => 700,
  'BubbleSort' => 1000,
  'QuickSort'  => 800,
  'TreeSort'   => 300,
  'Fannkuch'   => 9,
  'Mandelbrot' => 350,
}
# One outer iteration per trial; the inner already fills ~1 s of work.
# best-of-3 is taken across three trials in best_of_3, not via outer.
OUTER_OVERRIDE  = Hash.new(1)

# Engine columns in display order. The :cmd lambda receives (bench,
# iters_b, inner) and returns the shell command string for one trial.
COLUMNS = [
  { key: :interp, label: 'interp',
    cmd: ->(b, it, inn) { %(cd '#{ASOM_DIR}' && ./asom --plain -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '#{b}' #{it} #{inn}) } },
  { key: :aot,    label: 'aot',
    cmd: ->(b, it, inn) { %(cd '#{ASOM_DIR}' && ./asom -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '#{b}' #{it} #{inn}) } },
  { key: :pg,     label: 'pg',
    cmd: ->(b, it, inn) { %(cd '#{ASOM_DIR}' && ./asom -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '#{b}' #{it} #{inn}) } },
  { key: :sompp,  label: 'SOM++',
    cmd: ->(b, it, inn) { %(cd '#{SOMPP_DIR}' && ./build/SOM++ -cp 'Smalltalk:Examples/Benchmarks' Examples/Benchmarks/BenchmarkHarness.som '#{b}' #{it} #{inn}) } },
  { key: :truf,   label: 'Truffle',
    cmd: ->(b, it, inn) { %(cd '#{TRUFFLESOM_DIR}' && JVMCI_VERSION_CHECK=ignore PATH='#{MX_DIR}':$PATH ./som -cp 'Smalltalk:Examples/Benchmarks' Examples/Benchmarks/BenchmarkHarness.som '#{b}' #{it} #{inn}) } },
]

# Parse the engine's per-iter timings out of stdout, drop the warmup
# half, and return the mean of the remaining (warm) iterations in
# seconds. Returns nil if no recognisable line was found.
#
# Both asom Bench.som and SOM-st BenchmarkHarness.som print per-outer-
# iter lines like:
#   "<bench>: iterations=1 runtime: <Y>us"   (one per outer iter)
# followed by a summary:
#   "<bench>: iterations=<N> average: <X>us total: <Y>us"
#
# We collect the per-iter values, drop the first floor(N/2) (Truffle
# typically needs 2-3 outer iters to reach warm peak; SOM++/asom are
# warmup-irrelevant so dropping iters is harmless to them). With the
# default ITERS=5 we drop 2 and average the last 3.
def parse_inner(text)
  per = text.scan(/iterations=1 runtime: (\d+)us/).map { |m| m[0].to_i }
  if per.size >= 2
    warm = per.drop(per.size / 2)
    return (warm.sum.to_f / warm.size) / 1e6
  end
  # Single-iter run: fall back to the summary "total" line.
  if text =~ /average: \d+us total: (\d+)us/
    return $1.to_i / 1e6
  end
  if text =~ /iterations=\d+ runtime: (\d+)us/
    return $1.to_i / 1e6
  end
  nil
end

# Run one process under /usr/bin/time. Captures stdout + stderr together
# and the wall-clock %e from time. Returns [inner_seconds, wall_seconds];
# either may be nil on parse failure.
def single_trial(cmd)
  Tempfile.create('asom-bench') do |out|
    out.close
    # /usr/bin/time prints "%e\n" to stderr after the wrapped command
    # finishes. Merge stderr -> stdout for parsing convenience, then
    # split: the last line is the wall time, everything before is the
    # engine's own output.
    full = `/usr/bin/time -f '%e' bash -c "#{cmd.gsub('"', '\"')}" 2>&1`
    lines = full.split("\n")
    wall = lines.pop&.strip&.then { |s| s =~ /\A\d+(\.\d+)?\z/ ? s.to_f : nil }
    inner = parse_inner(lines.join("\n"))
    [inner, wall]
  end
end

# best-of-3: run trials 3x and pick min independently per metric.
def best_of_3(cmd)
  bi = nil
  bw = nil
  3.times do
    inner, wall = single_trial(cmd)
    bi = inner if inner && (bi.nil? || inner < bi)
    bw = wall  if wall  && (bw.nil? || wall  < bw)
  end
  [bi, bw]
end

def fmt(v)
  v ? format('%.3f', v) : '?'
end

# Run AOT bake + cached run for asom. AOT/PG share the same `./asom -cp
# ... Bench` invocation; the difference is which baking step runs first.
def asom_aot_warmup(bench, inner)
  cs = "#{ASOM_DIR}/code_store"
  FileUtils.rm_rf(cs)
  cp = ['SOM/Smalltalk', 'SOM/Examples/Benchmarks', 'test', '.']
         .map { |d| "#{ASOM_DIR}/#{d}" }.join(':')
  system({ 'CCACHE_DISABLE' => '1' },
         "#{ASOM_DIR}/asom", '-c', "--preload=#{bench}", '-cp', cp,
         'Bench', bench, '1', inner.to_s,
         out: File::NULL, err: File::NULL)
end

def asom_pg_warmup(bench, inner)
  cs = "#{ASOM_DIR}/code_store"
  FileUtils.rm_rf(cs)
  cp = ['SOM/Smalltalk', 'SOM/Examples/Benchmarks', 'test', '.']
         .map { |d| "#{ASOM_DIR}/#{d}" }.join(':')
  system({ 'CCACHE_DISABLE' => '1' },
         "#{ASOM_DIR}/asom", '-p', '--pg-threshold=1', "--preload=#{bench}",
         '-cp', cp,
         'Bench', bench, '5', inner.to_s,
         out: File::NULL, err: File::NULL)
end

inner_table = {}
wall_table  = {}

BENCHES.each do |b|
  inner   = INNER[b]
  iters_b = OUTER_OVERRIDE.fetch(b, ITERS)
  inner_row = {}
  wall_row  = {}

  COLUMNS.each do |col|
    # AOT/PG warmups go before measuring those columns.
    case col[:key]
    when :aot then asom_aot_warmup(b, inner)
    when :pg  then asom_pg_warmup(b, inner)
    end

    bi, bw = best_of_3(col[:cmd].call(b, iters_b, inner))
    inner_row[col[:key]] = bi
    wall_row[col[:key]]  = bw
  end

  # Reset the code store so a future `make bench` doesn't see stale SDs.
  FileUtils.rm_rf("#{ASOM_DIR}/code_store")

  inner_table[b] = inner_row
  wall_table[b]  = wall_row
  $stderr.puts "done #{b}"
end

def emit_table(title, table)
  cols = COLUMNS.map { |c| c[:label] }
  puts
  puts "# #{title}"
  printf("%-12s", 'benchmark')
  cols.each { |c| printf(' | %9s', c) }
  puts
  puts '-' * (12 + cols.size * 12)
  BENCHES.each do |b|
    printf("%-12s", b)
    COLUMNS.each { |c| printf(' | %9s', fmt(table[b][c[:key]])) }
    puts
  end
end

emit_table('Inner-work seconds  (engine system ticks; excludes startup / parse / JIT compile)',
           inner_table)
emit_table("Wall-clock seconds  (full process; /usr/bin/time -f '%e')",
           wall_table)
