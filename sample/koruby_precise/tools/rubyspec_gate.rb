#!/usr/bin/env ruby
# rubyspec compat gate: run the real rubyspec core suite through koruby's mspec
# shim and compare against a committed baseline (tools/rubyspec_baseline.txt).
#
#   ruby tools/rubyspec_gate.rb            # gate: fail (exit 1) on any regression
#   ruby tools/rubyspec_gate.rb --update   # re-record the baseline (after review)
#
# A regression = a file whose pass count dropped, whose fail/err count rose, or
# that newly whole-file-crashes.  Improvements are reported (and, with --update,
# folded into the baseline).  100% is NOT the goal: the baseline encodes the
# known-supported set (encoding/regex/IO specs stay failing by design); the gate
# only guards against *going backwards*.
require 'open3'
HERE     = File.expand_path('..', __dir__)
BASELINE = "#{HERE}/tools/rubyspec_baseline.txt"
CUR      = "#{ENV['TMPDIR'] || '/tmp'}/rubyspec_cur_#{Process.pid}.txt"
update   = ARGV.include?('--update')

def parse(path)
  h = {}
  return h unless File.file?(path)
  File.foreach(path) do |ln|
    f, *rest = ln.chomp.split("\t")
    h[f] = (rest[0] == 'WFAIL') ? [:WFAIL, rest[1].to_i] : rest.map(&:to_i)   # [pass,fail,err,skip] | [:WFAIL,code]
  end
  h
end

# Run the suite, dumping current per-file results.
env = { 'DUMP' => CUR }
system(env, RbConfig.ruby, "#{HERE}/tools/rubyspec_run.rb", *ARGV.reject { |a| a == '--update' }) or abort('runner failed')
cur  = parse(CUR); File.delete(CUR) if File.exist?(CUR)
base = parse(BASELINE)

# Non-deterministic / environment-dependent files are ignored for regression
# detection (still counted in the pass-rate by the runner).
EXCLUDE = File.file?("#{HERE}/tools/rubyspec_exclude.txt") ?
  File.readlines("#{HERE}/tools/rubyspec_exclude.txt").map(&:strip).reject { |l| l.empty? || l.start_with?('#') } : []
excluded = ->(f) { EXCLUDE.any? { |p| f.start_with?(p) } }

# A real regression moves examples pass -> fail/err, i.e. fail+err RISES while pass
# does NOT rise.  Pure pass-count drops with unchanged fail+err are be_computed_by
# expansion variance (some specs generate a different example count run to run, e.g.
# tables that iterate Encoding.list) — not a regression.  An improvement is more passes.
BAD_TOL = 2
def bad(v);  v[0] == :WFAIL ? 1_000_000 : v[1] + v[2]; end
def pass(v); v[0] == :WFAIL ? -1 : v[0]; end

regressions = []; improvements = []; newfiles = []
cur.each do |f, v|
  next if excluded.(f)
  b = base[f]
  if !b
    newfiles << f
  elsif (v[0] == :WFAIL && b[0] != :WFAIL) || (bad(v) - bad(b) > BAD_TOL && pass(v) <= pass(b))
    regressions << [f, b, v]
  elsif pass(v) > pass(b) || (b[0] == :WFAIL && v[0] != :WFAIL)
    improvements << [f, b, v]
  end
end
gone = base.keys - cur.keys

puts "\n=== rubyspec compat gate ==="
puts "baseline files: #{base.size}   current files: #{cur.size}"
unless improvements.empty?
  puts "\nIMPROVED (#{improvements.size}):"
  improvements.first(40).each { |f, b, c| puts "  #{f}: #{b.inspect} -> #{c.inspect}" }
end
puts "\nNEW files not in baseline (#{newfiles.size}): #{newfiles.first(10).join(', ')}" unless newfiles.empty?
puts "\nDISAPPEARED from run (#{gone.size}): #{gone.first(10).join(', ')}" unless gone.empty?

if regressions.empty?
  puts "\nNO REGRESSIONS ✓"
  if update && (!improvements.empty? || !newfiles.empty?)
    File.write(BASELINE, cur.sort.map { |f, v| v[0] == :WFAIL ? "#{f}\tWFAIL\t#{v[1]}" : "#{f}\t#{v.join("\t")}" }.join("\n") + "\n")
    puts "baseline updated (#{cur.size} entries)."
  end
  exit 0
else
  puts "\nREGRESSIONS (#{regressions.size}):"
  regressions.each { |f, b, c| puts "  #{f}: #{b.inspect} -> #{c.inspect}" }
  # Never fold a real regression into the baseline, even with --update: investigate
  # first (add to the exclude list if it's flaky, or fix the code).
  puts "\nbaseline NOT updated — resolve the regressions (or exclude if flaky) first." if update
  exit 1
end
