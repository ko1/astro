# Run a set of rubyspec files through the real mspec framework and print
# "rel  ex F E  pass".  Usage: ruby tools/sw.rb <jobs> <relpath-or-glob>...
require 'open3'
require 'fileutils'
require 'thread'
K       = ENV['KORUBY'] || File.expand_path("../koruby_precise", __dir__)
SPECDIR = "#{ENV['HOME']}/ruby/src/master/spec/ruby"
TMPBASE = "#{ENV['TMPDIR'] || '/tmp'}/koruby_sw_#{Process.pid}"
LAUNCHER = File.expand_path("mspec_launch.rb", __dir__)
FileUtils.rm_rf(TMPBASE); FileUtils.mkdir_p(TMPBASE)
jobs = (ARGV.shift || 8).to_i
files = ARGV.flat_map { |a|
  p0 = a.start_with?("/") ? a : "#{SPECDIR}/#{a}"
  File.directory?(p0) ? Dir.glob("#{p0}/**/*_spec.rb") : Dir.glob(p0)
}.uniq.sort
RE = /(\d+) examples?, \d+ expectations?, (\d+) failures?, (\d+) errors?/
q = Queue.new; files.each { |f| q << f }
mu = Mutex.new; res = {}
Array.new([jobs, files.size].min) {
  Thread.new do
    while (f = (q.pop(true) rescue nil))
      rel = f.sub("#{SPECDIR}/", "")
      td  = "#{TMPBASE}/#{rel.gsub(%r{[/.]}, '_')}"
      FileUtils.mkdir_p(td)
      log = "#{td}/.out"
      pid = Process.spawn({ 'MSPEC_RUNNER' => '1', 'SPEC_TEMP_DIR' => td },
                          "timeout", "-k", "5", "30", K, LAUNCHER, f,
                          chdir: SPECDIR, in: "/dev/null", out: log, err: [:child, :out])
      _, _st = Process.waitpid2(pid)
      out = File.exist?(log) ? File.binread(log) : ""
      FileUtils.rm_rf(td)
      m = RE.match(out.dup.force_encoding("UTF-8").scrub)
      mu.synchronize { res[rel] = m ? [m[1].to_i, m[2].to_i, m[3].to_i] : nil }
    end
  end
}.each(&:join)
FileUtils.rm_rf(TMPBASE)
tp = tf = te = 0
res.keys.sort.each do |rel|
  r = res[rel]
  if r
    ex, fa, er = r
    pass = ex - fa - er
    tp += pass; tf += fa; te += er
    puts "%-58s ex=%-5d F=%-4d E=%-4d pass=%d" % [rel, ex, fa, er, pass]
  else
    puts "%-58s WFAIL" % rel
  end
end
puts "TOTAL pass=#{tp} fail=#{tf} err=#{te}"
