#!/usr/bin/env ruby
# Compile all test expressions into code store for test-compiled mode.

require 'rbconfig'
require_relative '../lib/abruby'

src_dir = File.expand_path('..', __dir__)
store_dir = File.join(src_dir, 'code_store')

so_mtime = File.mtime(File.join(src_dir, 'abruby.so')).to_i rescue 0
AbRuby.cs_init(store_dir, src_dir, so_mtime)

cflags = "-I#{RbConfig::CONFIG['rubyhdrdir']} -I#{RbConfig::CONFIG['rubyarchhdrdir']}"

dir = File.dirname(__FILE__)
files = Dir.glob(File.join(dir, "test_*.rb")).sort

vm = AbRuby.new

files.each do |f|
  src = File.read(f)
  # Extract eval strings from assert_eval calls
  src.scan(/assert_eval\s*\(\s*(['"])(.*?)\1/m) do |_q, code|
    begin
      ast = vm.parse(code)
      AbRuby.cs_compile(ast)
      vm.last_entries.each { |_name, body| AbRuby.cs_compile(body) }
    rescue Exception
    end
  end
  # Also try multi-line heredoc-style strings
  src.scan(/assert_eval\s*\(\s*<<~?['"]?(\w+)['"]?\s*\n(.*?)\n\s*\1/m) do |_tag, code|
    begin
      ast = vm.parse(code)
      AbRuby.cs_compile(ast)
      vm.last_entries.each { |_name, body| AbRuby.cs_compile(body) }
    rescue Exception
    end
  end
end

AbRuby.cs_build(cflags)
entries = Dir.glob(File.join(store_dir, 'c', 'SD_*.c')).size
puts "compiled #{entries} entries"
