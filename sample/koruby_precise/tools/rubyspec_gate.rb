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

def score(v)  # comparable "goodness": crash is worst, else pass count minus penalties
  return -1_000_000 if v[0] == :WFAIL
  v[0] - (v[1] + v[2]) * 1000   # heavily weight fail/err so a pass->fail is always a regression
end

regressions = []; improvements = []; newfiles = []
cur.each do |f, v|
  next if excluded.(f)
  if !base.key?(f)
    newfiles << f
  elsif score(v) < score(base[f])
    regressions << [f, base[f], v]
  elsif score(v) > score(base[f])
    improvements << [f, base[f], v]
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
  if update
    File.write(BASELINE, cur.sort.map { |f, v| v[0] == :WFAIL ? "#{f}\tWFAIL\t#{v[1]}" : "#{f}\t#{v.join("\t")}" }.join("\n") + "\n")
    puts "\nbaseline force-updated despite regressions (--update)."
    exit 0
  end
  exit 1
end
