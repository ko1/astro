# 本物の mspec framework で rubyspec を回す runner (shim 不使用)。
# 使い方: ruby tools/mspec_real_run.rb <spec-dir> <jobs> [DUMP=per-file.tsv]
# per-file 20s timeout。summary 行を集計。shim (rubyspec_run.rb) より正確 (it_behaves_like/mock が本物)。
# Prototype: run real rubyspec files through the REAL mspec framework via koruby,
# one file per koruby process (isolation), tally the summary line.
# Usage: ruby mspec_run.rb <spec-root-dir> <jobs> [DUMP=path]
require 'open3'
require 'fileutils'
K    = ENV['KORUBY'] || "/home/ko1/ruby/astro/sample/koruby_precise/koruby_precise"
# mspec's default temp dir is <cwd>/rubyspec_temp, i.e. inside the (possibly
# read-only) rubyspec checkout — every file/io spec then dies in `before :each`
# with EROFS.  Give each spawned process its own writable dir instead.
TMPBASE = ENV['SPEC_TEMP_BASE'] || "#{ENV['TMPDIR'] || '/tmp'}/koruby_spec_tmp"
FileUtils.rm_rf(TMPBASE)
FileUtils.mkdir_p(TMPBASE)
root = ARGV[0] || "#{ENV['HOME']}/ruby/src/master/spec/ruby/core"
jobs = (ARGV[1] || 16).to_i
specdir = "#{ENV['HOME']}/ruby/src/master/spec/ruby"
files = Dir.glob("#{root}/**/*_spec.rb").sort
# Two ways to run a spec file:
#   launch (default) — drive MSpecRun ourselves so the spec file stays at top
#                      level and its require_relative chain finishes first.
#   self             — let ruby/spec's spec_helper self-run (`MSpecRun.main`).
#                      Broken for specs behind an intermediate spec_helper that
#                      requires a library; see tools/mspec_launch.rb.
LAUNCHER = File.expand_path("mspec_launch.rb", __dir__)
if ENV['MSPEC_MODE'] == 'self'
  SPAWN_ENV = { 'MSPEC_RUNNER' => nil }
  ARGS_FOR  = ->(f) { [f] }
else
  SPAWN_ENV = { 'MSPEC_RUNNER' => '1' }
  ARGS_FOR  = ->(f) { [LAUNCHER, f] }
end
# summary line: "1 file, N examples, M expectations, F failures, E errors, T tagged"
RE = /(\d+) examples?, \d+ expectations?, (\d+) failures?, (\d+) errors?/
require 'thread'
q = Queue.new; files.each { |f| q << f }
mutex = Mutex.new
tot = Hash.new(0); per = {}
workers = Array.new([jobs, files.size].min) do
  Thread.new do
    while (f = (q.pop(true) rescue nil))
      rel = f.sub("#{specdir}/", "")
      td = "#{TMPBASE}/#{rel.gsub(%r{[/.]}, '_')}"
      FileUtils.mkdir_p(td)
      # Capture through a file, not a pipe: a spec that spawns a child (which
      # inherits the capture descriptor) would otherwise keep the pipe open past
      # the timeout kill and hang the read forever.
      log = "#{td}/.out"
      pid = Process.spawn(SPAWN_ENV.merge('SPEC_TEMP_DIR' => td),
                          "timeout", "-k", "5", "20", K, *ARGS_FOR.call(f),
                          chdir: specdir, in: "/dev/null", out: log, err: [:child, :out])
      _, st = Process.waitpid2(pid)
      out = File.exist?(log) ? File.binread(log) : ""
      FileUtils.rm_rf(td)
      m = RE.match(out.dup.force_encoding("UTF-8").scrub)
      mutex.synchronize do
        tot[:files] += 1
        if m
          ex, fa, er = m[1].to_i, m[2].to_i, m[3].to_i
          pass = ex - fa - er
          per[rel] = [pass, fa, er]
          tot[:ex] += ex; tot[:pass] += pass; tot[:fail] += fa; tot[:err] += er
          tot[:clean] += 1 if fa == 0 && er == 0 && ex > 0
        else
          per[rel] = [:WFAIL, st.exitstatus || -1]   # no summary printed = crash/timeout/parse-fail
          tot[:wfail] += 1
        end
      end
    end
  end
end
workers.each(&:join)
puts "files=#{tot[:files]} clean=#{tot[:clean]} whole-file-fail=#{tot[:wfail]}"
puts "examples=#{tot[:ex]} pass=#{tot[:pass]} fail=#{tot[:fail]} err=#{tot[:err]}"
denom = tot[:pass] + tot[:fail] + tot[:err]
puts "example pass-rate = #{denom > 0 ? (100.0 * tot[:pass] / denom).round(1) : 0}%"
if (dp = ENV['DUMP'])
  File.open(dp, 'w') do |io|
    per.sort.each { |f, v| io.puts(v[0] == :WFAIL ? "#{f}\tWFAIL\t#{v[1]}" : "#{f}\t#{v[0]}\t#{v[1]}\t#{v[2]}") }
  end
  puts "wrote #{per.size} entries to #{dp}"
end
