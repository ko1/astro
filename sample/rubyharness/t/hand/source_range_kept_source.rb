# Location#source_range for eval code (kept only under RubyVM.keep_script_lines), and for a file changed or
# removed after it was loaded.
def range_of(loc)
  r = loc.source_range
  [r.path, r.absolute_path, r.start_line, r.start_column, r.end_line, r.end_column]
rescue Exception => e
  [e.class, e.message]
end

RubyVM.keep_script_lines = true
begin
  eval("x = 1\nnil.foo", binding, "kept_eval.rb", 100)
rescue NoMethodError => e
  p range_of(e.backtrace_locations[0])
end
begin
  eval("f = lambda { |a| }\nf.call", binding, "kept_eval2.rb", 10)
rescue ArgumentError => e
  p range_of(e.backtrace_locations[0])
end
RubyVM.keep_script_lines = false
begin
  eval("nil.foo", binding, "dropped_eval.rb")
rescue NoMethodError => e
  p range_of(e.backtrace_locations[0])
end

dir = File.join(Dir.pwd, "tmp_source_range_#{Process.pid}")
Dir.mkdir(dir)
begin
  path = File.join(dir, "loaded.rb")
  [["nil.foo\n", nil], ["nil.foo\n", "nil.longer_name\n"], ["nil.foo\n", "(\n"], ["nil.foo\n", :remove]].each do |src, change|
    File.binwrite(path, src)
    loc = begin
      load path
    rescue NoMethodError => e
      e.backtrace_locations[0]
    end
    if change == :remove
      File.delete(path)
    elsif change
      File.binwrite(path, change)
    end
    r = range_of(loc)
    r[0] = File.basename(r[0]) if r[0].is_a?(String)
    r[1] = File.basename(r[1]) if r[1].is_a?(String)
    p r
  end
ensure
  Dir.glob(File.join(dir, "*")).each { |f| File.delete(f) }
  Dir.rmdir(dir)
end
