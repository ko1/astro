# 本物の mspec framework で rubyspec を回す runner (shim 不使用)。
# 使い方: ruby tools/mspec_real_run.rb <spec-dir> <jobs> [DUMP=per-file.tsv]
# per-file 20s timeout。summary 行を集計。shim (rubyspec_run.rb) より正確 (it_behaves_like/mock が本物)。
# Prototype: run real rubyspec files through the REAL mspec framework via koruby,
# one file per koruby process (isolation), tally the summary line.
# Usage: ruby mspec_run.rb <spec-root-dir> <jobs> [DUMP=path]
require 'open3'
K    = ENV['KORUBY'] || "/home/ko1/ruby/astro/sample/koruby_precise/koruby_precise"
root = ARGV[0] || "#{ENV['HOME']}/ruby/src/master/spec/ruby/core"
jobs = (ARGV[1] || 16).to_i
specdir = "#{ENV['HOME']}/ruby/src/master/spec/ruby"
files = Dir.glob("#{root}/**/*_spec.rb").sort
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
      out, st = Open3.capture2e({ 'MSPEC_RUNNER' => nil }, "timeout", "20", K, f, chdir: specdir, stdin_data: "")
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
