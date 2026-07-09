#!/usr/bin/env ruby
# Run ONE rubyspec file through the shim and print FAIL/ERR detail lines.
# Usage: ruby tools/runspec1.rb <spec_rel_or_abs> [--cruby]
# --cruby: run the same concatenated file through the real `ruby` for comparison.
require 'open3'
HERE = File.expand_path('..', __dir__)
BIN  = "#{HERE}/koruby_precise"
SHIM = File.read("#{HERE}/tools/mspec_shim.rb")
TRL  = File.read("#{HERE}/tools/mspec_trailer.rb")
CORE = "#{ENV['HOME']}/ruby/src/master/spec/ruby/core"
arg  = ARGV[0] or abort("usage: runspec1.rb <spec>")
path = File.file?(arg) ? arg : "#{CORE}/#{arg}"
path = "#{path}_spec.rb" unless path.end_with?('.rb')
abort("no such spec: #{path}") unless File.file?(path)

def resolve_requires(path, seen)
  return "" if seen.include?(path) || !File.file?(path)
  seen << path
  src = File.binread(path).force_encoding('UTF-8')
  out = +"".b.force_encoding('UTF-8')
  src.scrub.scan(/^\s*require_relative\s+['"]([^'"]+)['"]/) do |rel,|
    next if rel =~ /spec_helper|\bmspec\b/
    dep = File.expand_path(rel, File.dirname(path)) + ".rb"
    out << resolve_requires(dep, seen)
  end
  out << src << "\n"
  out
end

body = resolve_requires(path, [])
full = "#{SHIM}\n#{body}\n#{TRL}"
tmp = "#{ENV['TMPDIR'] || '/tmp'}/runspec1_#{Process.pid}.rb"
File.write(tmp, full)
if ARGV.include?('--cruby')
  out, st = Open3.capture2e('ruby', tmp)
  puts out
else
  out, st = Open3.capture2e(BIN, tmp)
  puts out
  puts "exit=#{st.exitstatus}"
end
File.delete(tmp) if File.exist?(tmp)
