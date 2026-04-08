#!/usr/bin/env ruby
# Pre-compile all benchmark files into code store.
# Run this before benchmarking with abruby/compiled runner.

require 'rbconfig'
require_relative '../lib/abruby'

src_dir = File.expand_path('..', __dir__)
store_dir = File.join(src_dir, 'code_store')
cflags = "-I#{RbConfig::CONFIG['rubyhdrdir']} -I#{RbConfig::CONFIG['rubyarchhdrdir']}"

AbRuby.cs_init(store_dir, src_dir)

files = Dir.glob(File.join(__dir__, 'bm_*.ab.rb')).sort
vm = AbRuby.new

files.each do |f|
  name = File.basename(f)
  begin
    ast = vm.parse(File.read(f), f)
    AbRuby.cs_compile(ast)
    vm.last_entries.each do |entry_name, body|
      AbRuby.cs_compile(body)
    end
    puts "  compiled: #{name} (#{vm.last_entries.size + 1} entries)"
  rescue Exception => e
    puts "  skipped:  #{name} (#{e.message})"
  end
end

AbRuby.cs_build(cflags)
entries = Dir.glob(File.join(store_dir, 'c', 'SD_*.c')).size
puts "#{entries} entries in #{store_dir}"
