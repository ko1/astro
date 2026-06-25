#!/usr/bin/env ruby
# Run rubyspec spec files through the koruby_precise mspec shim and aggregate
# pass/fail/err/skip.  Usage: ruby tools/rubyspec_run.rb <spec_dir> [jobs]
# Model: koruby has no ARGV, so each run = (shim + spec + trailer) concatenated
# into one temp file, executed by ./koruby_precise.  The trailer prints
# `pass=N fail=N err=N skip=N`.
require 'open3'
HERE = File.expand_path('..', __dir__)
BIN  = "#{HERE}/koruby_precise"
SHIM = File.read("#{HERE}/tools/mspec_shim.rb")
TRL  = File.read("#{HERE}/tools/mspec_trailer.rb")
dir  = ARGV[0] || "#{ENV['HOME']}/ruby/src/master/spec/ruby/core"
jobs = (ARGV[1] || 16).to_i
files = Dir.glob("#{dir}/**/*_spec.rb").sort

# Resolve require_relative fixtures/shared files (koruby has no working require),
# concatenating them before the spec so helper classes/shared blocks are defined.
# spec_helper / mspec / fixtures that pull in mspec are skipped (the shim covers them).
def resolve_requires(path, seen)
  return "" if seen.include?(path) || !File.file?(path)
  seen << path
  src = File.read(path)
  out = +""
  src.scan(/^\s*require_relative\s+['"]([^'"]+)['"]/) do |rel,|
    next if rel =~ /spec_helper|\bmspec\b/
    dep = File.expand_path(rel, File.dirname(path)) + ".rb"
    out << resolve_requires(dep, seen)
  end
  out << src << "\n"
  out
end

tot = Hash.new(0); per_file = {}
q = Queue.new; files.each { |f| q << f }
mutex = Mutex.new
workers = Array.new([jobs, files.size].min) do |w|
  Thread.new do
    tmp = "#{ENV['TMPDIR'] || '/tmp'}/rsr_#{Process.pid}_#{w}.rb"
    loop do
      f = (q.pop(true) rescue break)
      File.write(tmp, SHIM + "\n" + resolve_requires(f, []) + "\n" + TRL)
      out, _e, st = Open3.capture3('timeout', '-k', '2', '25', BIN, tmp)
      code = st.exited? ? st.exitstatus : (128 + (st.termsig || 0))
      m = out.lines.reverse.find { |l| l =~ /\Apass=(\d+) fail=(\d+) err=(\d+) skip=(\d+)/ }
      rel = f.sub(dir + '/', '')
      if m && m =~ /pass=(\d+) fail=(\d+) err=(\d+) skip=(\d+)/
        p_,fl,er,sk = $1.to_i,$2.to_i,$3.to_i,$4.to_i
        mutex.synchronize { tot[:pass]+=p_; tot[:fail]+=fl; tot[:err]+=er; tot[:skip]+=sk; tot[:files]+=1
          tot[:filepass]+=1 if fl==0 && er==0
          per_file[rel] = [p_,fl,er,sk] }
      else
        # crash / no trailer = whole-file failure (parse error / SEGV / timeout)
        mutex.synchronize { tot[:wfail]+=1; per_file[rel] = [:WFAIL, code] }
      end
    end
    File.delete(tmp) if File.exist?(tmp)
  end
end
workers.each(&:join)
ex = tot[:pass]+tot[:fail]+tot[:err]
puts "files=#{tot[:files]} (file-clean=#{tot[:filepass]})  whole-file-fail/crash=#{tot[:wfail]}"
puts "examples: pass=#{tot[:pass]} fail=#{tot[:fail]} err=#{tot[:err]} skip=#{tot[:skip]}"
puts "example pass-rate (of pass+fail+err) = #{ex>0 ? (100.0*tot[:pass]/ex).round(1) : 0}%"
# worst files
if ENV['WORST']
  puts "\n# worst (fail+err):"
  per_file.select{|_,v| v.is_a?(Array) && v[0]!=:WFAIL && (v[1]+v[2])>0}.sort_by{|_,v| -(v[1]+v[2])}.first(30).each{|f,v| puts "  #{f}: pass=#{v[0]} fail=#{v[1]} err=#{v[2]}"}
  puts "\n# whole-file fail/crash:"
  per_file.select{|_,v| v.is_a?(Array)&&v[0]==:WFAIL}.first(30).each{|f,v| puts "  #{f}: code=#{v[1]}"}
end
