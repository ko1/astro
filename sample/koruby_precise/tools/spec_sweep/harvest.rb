# Harvest rubyspec failure BODIES (not just counts) and bucket them by a
# normalized signature, so one root cause scattered over many files shows up as
# one row instead of N one-example files.
#
#   ruby tools/spec_sweep/harvest.rb <jobs> <out.tsv-from-sweep> <raw.json> [minF] [maxF]
#
# Reads a sweep TSV to pick which files to re-run (default: the ones with
# 1..2 failing examples -- the long tail), re-runs each through the real mspec
# launcher, and splits the report into per-example blocks.
#
# Driven by CRuby; the spawned processes are koruby.  SWEEP_TMP must be short
# (see sweep.rb) or library/socket self-destructs on sun_path length.
require 'json'
require 'fileutils'
require 'thread'

K        = ENV['KORUBY'] || File.expand_path("../../koruby_precise", __dir__)
SPECDIR  = ENV['SPECDIR'] || "#{ENV['HOME']}/ruby/src/master/spec/ruby"
LAUNCHER = File.expand_path("../mspec_launch.rb", __dir__)
TMPBASE  = "#{ENV['SWEEP_TMP'] || '/tmp/ksw'}/koruby_harvest_#{Process.pid}"
TIMEOUT  = (ENV['SPEC_TIMEOUT'] || 30).to_i

# Out of scope for this campaign (concurrency / GC introspection / harness).
OUT = %w[core/tracepoint core/fiber core/thread core/mutex core/objectspace
         core/gc ractor refine ruby2_keywords]

jobs = (ARGV.shift || 6).to_i
tsv  = ARGV.shift or abort "usage: harvest.rb <jobs> <sweep.tsv> <raw.json> [minF] [maxF]"
raw  = ARGV.shift or abort "usage: harvest.rb <jobs> <sweep.tsv> <raw.json> [minF] [maxF]"
minf = (ARGV.shift || 1).to_i
maxf = (ARGV.shift || 2).to_i

files = File.readlines(tsv).filter_map { |l|
  c = l.chomp.split("\t")
  next if c.size < 5 || c[1] == "WFAIL"
  next if OUT.any? { |o| c[0].start_with?(o) }
  n = c[2].to_i + c[3].to_i
  (n >= minf && n <= maxf) ? c[0] : nil
}
abort "no files selected" if files.empty?
$stderr.puts "harvesting #{files.size} files (#{minf}..#{maxf} failing examples)"

# One mspec report block looks like:
#   \nN)\n<description> FAILED|ERROR\n<message lines>\n<backtrace lines>\n
# The backtrace is the trailing run of lines that start with a path or "\t".
def split_blocks(txt)
  out = []
  # Blocks are separated by a line that is just "N)".
  parts = txt.split(/^\d+\)\n/)
  parts.shift                                   # preamble (dots / banner)
  parts.each do |p|
    p = p.sub(/\nFinished in .*\z/m, '')
    lines = p.lines.map(&:chomp)
    lines.pop while !lines.empty? && lines.last.strip.empty?
    next if lines.empty?
    head = lines.shift.to_s
    kind = head =~ /\s(FAILED|ERROR)\z/ ? $1 : "?"
    desc = head.sub(/\s(FAILED|ERROR)\z/, '')
    # Trailing backtrace: lines that look like "<path>:<n>:in '...'" or start with /
    bt = []
    bt.unshift(lines.pop) while !lines.empty? && lines.last =~ %r{\A(/|\S+:\d+:in )}
    out << { kind: kind, desc: desc, msg: lines.join("\n"), bt: bt }
  end
  out
end

FileUtils.rm_rf(TMPBASE); FileUtils.mkdir_p(TMPBASE)
q = Queue.new; files.each { |f| q << f }
mu = Mutex.new; res = []; done = 0
t0 = Time.now
Array.new([jobs, files.size].min) {
  Thread.new do
    while (rel = (q.pop(true) rescue nil))
      td = "#{TMPBASE}/#{rel.gsub(%r{[/.]}, '_')}"
      FileUtils.mkdir_p(td)
      log = "#{td}/.out"
      pid = Process.spawn({ 'MSPEC_RUNNER' => '1', 'RUBY_FLAGS' => '',
                            'SPEC_TEMP_DIR' => td },
                          "timeout", "-k", "5", TIMEOUT.to_s, K, LAUNCHER,
                          "#{SPECDIR}/#{rel}",
                          chdir: SPECDIR, in: "/dev/null", out: log, err: [:child, :out])
      Process.waitpid2(pid)
      txt = File.exist?(log) ? File.binread(log).force_encoding("UTF-8").scrub : ""
      FileUtils.rm_rf(td)
      blocks = split_blocks(txt)
      mu.synchronize do
        blocks.each { |b| res << b.merge(rel: rel) }
        done += 1
        $stderr.printf("\r%d/%d %.0fs", done, files.size, Time.now - t0) if done % 25 == 0
      end
    end
  end
}.each(&:join)
FileUtils.rm_rf(TMPBASE)
$stderr.puts

File.write(raw, JSON.pretty_generate(res))
$stderr.puts "wrote #{res.size} failure blocks to #{raw}  (#{(Time.now - t0).round}s)"
