#!/usr/bin/env ruby
# koruby benchmark runner.  Modeled on sample/abruby/benchmark/run.rb.

require 'optparse'

KORUBY_DIR = File.expand_path('..', __dir__)
BENCHMARK_DIR = __dir__
ABRUBY_DIR = File.expand_path('../../abruby', __dir__)
STORE = "#{KORUBY_DIR}/code_store"

RUNNERS = [
  { name: 'ruby',         cmd: 'ruby %s' },
  { name: 'ruby+yjit',    cmd: 'ruby --yjit %s' },
  { name: 'abruby+pgc',   cmd: "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --aot --pg-compile %s",
    setup: proc {} },  # warm-up: nothing required — abruby populates its own store
  { name: 'koruby+aot',   cmd: "env KORUBY_CODE_STORE=#{STORE} #{KORUBY_DIR}/koruby %s",
    setup: proc { |bench|
      system("rm -rf #{STORE}")
      system("#{KORUBY_DIR}/koruby --aot-compile #{bench} >/dev/null 2>&1")
    }},
]

def time_run(cmd)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out = `#{cmd} 2>&1`
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  [t1 - t0, out.strip.lines.last.to_s.strip]
end

def main(argv)
  iters = 3
  selected = nil
  runner_filter = nil
  warmup = true
  OptionParser.new do |o|
    o.on('-n N', Integer, 'iterations (best-of-N)') { |n| iters = n }
    o.on('-r RUNNERS', String) { |r| runner_filter = r.split(',') }
    o.on('-l', 'list benches') {
      Dir["#{BENCHMARK_DIR}/bm_*.rb"].sort.each { |f| puts File.basename(f, '.rb').sub('bm_', '') }
      exit
    }
    o.on('--no-warmup') { warmup = false }
  end.parse!(argv)
  selected = argv unless argv.empty?

  benches = Dir["#{BENCHMARK_DIR}/bm_*.rb"].sort
  if selected
    benches = benches.select { |f| selected.include?(File.basename(f, '.rb').sub('bm_', '')) }
  end

  runners = RUNNERS
  runners = runners.select { |r| runner_filter.include?(r[:name]) } if runner_filter

  printf "%-20s", 'bench'
  runners.each { |r| printf " %14s", r[:name] }
  puts
  puts '-' * (20 + 15 * runners.size)

  benches.each do |bench|
    name = File.basename(bench, '.rb').sub('bm_', '')
    printf "%-20s", name
    runners.each do |r|
      r[:setup]&.call(bench)
      # warm
      if warmup
        `#{r[:cmd] % bench} >/dev/null 2>&1`
      end
      best = nil
      iters.times do
        t, _ = time_run(r[:cmd] % bench)
        best = t if best.nil? || t < best
      end
      printf " %14.3f", best
    end
    puts
  end
end

main(ARGV)
