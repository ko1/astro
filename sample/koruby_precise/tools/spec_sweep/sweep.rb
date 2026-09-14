# rubyspec sweep: run each spec file through the REAL mspec framework with the
# koruby binary as the interpreter, emit one TSV row per file.
#
#   koruby tools/spec_sweep/sweep.rb  -- no.  This script is driven by CRuby
#   (it only orchestrates); the *spawned* processes are koruby.
#
# Usage: ruby tools/spec_sweep/sweep.rb <jobs> <out.tsv> <relpath-or-glob>...
#
# SWEEP_TMP MUST BE A SHORT PATH (e.g. /tmp/ksw).  library/socket binds UNIX
# sockets under SPEC_TEMP_DIR, and sun_path is capped at ~108 bytes: a long base
# makes 19 files fail with errors that look like koruby gaps and are not.
# Measured 2026-09-13: long base -> socket pass 1700 / 59 errors; short base ->
# 1818 / 0.  CRuby run from the same long base fails identically, so "CRuby
# errors too" does NOT clear a socket result — shorten the path and re-measure.
#
# CLAMP pass AT ZERO WHEN TOTALLING.  A file that dies at load reports ex=0 with
# E=1, so pass = ex - F - E = -1.  124 such files (missing stdlib) currently
# subtract 124 from the total and understate the rate by 0.39 points.  Worse, a
# file that dies mid-load can report ex=0 E=83 = -83, which makes an unrelated
# change look like +/-83 examples.  The per-file column stays as computed; the
# TOTAL clamps.
#
# Report a rate as pass/ex FROM THE SAME RUN.  The example count moves run to
# run (be_computed_by tables expand differently), so dividing a new pass count
# by an old example count inflates the figure.
# TSV columns: rel  ex  F  E  pass        (pass = ex - F - E, WFAIL rows: rel WFAIL)
require 'fileutils'
require 'thread'

K        = ENV['KORUBY'] || File.expand_path("../../koruby_precise", __dir__)
SPECDIR  = ENV['SPECDIR'] || "#{ENV['HOME']}/ruby/src/master/spec/ruby"
LAUNCHER = File.expand_path("../mspec_launch.rb", __dir__)
# mspec deletes SPEC_TEMP_DIR on exit, so it must be re-created for every spawn.
TMPBASE  = "#{ENV['SWEEP_TMP'] || ENV['TMPDIR'] || '/tmp'}/koruby_sweep_#{Process.pid}"
TIMEOUT  = (ENV['SPEC_TIMEOUT'] || 30).to_i

jobs = (ARGV.shift || 4).to_i
out  = ARGV.shift or abort "usage: sweep.rb <jobs> <out.tsv> <path>..."
files = ARGV.flat_map { |a|
  p0 = a.start_with?("/") ? a : "#{SPECDIR}/#{a}"
  File.directory?(p0) ? Dir.glob("#{p0}/**/*_spec.rb") : Dir.glob(p0)
}.uniq.sort
abort "no spec files" if files.empty?

FileUtils.rm_rf(TMPBASE); FileUtils.mkdir_p(TMPBASE)
RE = /(\d+) examples?, \d+ expectations?, (\d+) failures?, (\d+) errors?/
q = Queue.new; files.each { |f| q << f }
mu = Mutex.new; res = {}; done = 0
t0 = Time.now
Array.new([jobs, files.size].min) {
  Thread.new do
    while (f = (q.pop(true) rescue nil))
      rel = f.sub("#{SPECDIR}/", "")
      td  = "#{TMPBASE}/#{rel.gsub(%r{[/.]}, '_')}"
      FileUtils.mkdir_p(td)
      log = "#{td}/.out"
      pid = Process.spawn({ 'MSPEC_RUNNER' => '1', 'RUBY_FLAGS' => '',
                            'SPEC_TEMP_DIR' => td },
                          "timeout", "-k", "5", TIMEOUT.to_s, K, LAUNCHER, f,
                          chdir: SPECDIR, in: "/dev/null", out: log, err: [:child, :out])
      _, _st = Process.waitpid2(pid)
      txt = File.exist?(log) ? File.binread(log) : ""
      FileUtils.rm_rf(td)
      m = RE.match(txt.dup.force_encoding("UTF-8").scrub)
      mu.synchronize do
        res[rel] = m ? [m[1].to_i, m[2].to_i, m[3].to_i] : nil
        done += 1
        $stderr.printf("\r%d/%d %.0fs", done, files.size, Time.now - t0) if done % 25 == 0
      end
    end
  end
}.each(&:join)
FileUtils.rm_rf(TMPBASE)
$stderr.puts

tp = tf = te = tw = 0
File.open(out, "w") do |io|
  res.keys.sort.each do |rel|
    if (r = res[rel])
      ex, fa, er = r
      pass = ex - fa - er
      tp += [pass, 0].max; tf += fa; te += er   # clamp: a load failure is ex=0/E=1 = -1 (see header)
      io.printf("%s\t%d\t%d\t%d\t%d\n", rel, ex, fa, er, pass)
    else
      tw += 1
      io.printf("%s\tWFAIL\t\t\t\n", rel)
    end
  end
end
puts "files=#{files.size} pass=#{tp} fail=#{tf} err=#{te} WFAIL=#{tw} elapsed=#{(Time.now - t0).round}s"
