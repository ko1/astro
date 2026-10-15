# C-method warnings carry the caller's file:line, Class.allocate#superclass, ARGF seek/rewind/
# readpartial edges, Kernel#gets reads ARGF (which shares ARGV) (2026-10-15)
def t(tag)
require "stringio"
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
def warns
  old = $stderr
  $stderr = StringIO.new
  v, $VERBOSE = $VERBOSE, true
  yield
  $stderr.string.gsub(__dir__ + "/", "")
ensure
  $stderr = old
  $VERBOSE = v
end
t(:init_blk) { warns { [1, 2].send(:initialize) { } } }
t(:new_blk) { warns { Array.new { } } }
t(:new_default_blk) { warns { Array.new(2, 1) { } } }
t(:count_blk) { warns { [1].count(1) { } } }
t(:any_blk) { warns { [1].any?(1) { } } }
t(:fetch_blk) { warns { {}.fetch(1, 2) { 3 } } }
t(:ary_fetch_blk) { warns { [].fetch(5, 0) { } } }
t(:format_extra) { warns { format("%d", 1, 2) } }
t(:re_ignorecase) { warns { Regexp.new("a", 1.5) } }
t(:attr_bool_quiet) { warns { Class.new { attr :x, true } } }
t(:alloc_super) { Class.allocate.superclass }
t(:basic_super) { [BasicObject.superclass, BasicObject.singleton_class.superclass] }
dir = File.join(__dir__, "argf_tmp_#{$$}")
Dir.mkdir(dir)
f1 = File.join(dir, "a.txt"); File.write(f1, "a1\na2\n")
f2 = File.join(dir, "b.txt"); File.write(f2, "b1\n")
begin
  t(:argv_shared) { ARGF.argv.equal?(ARGV) }
  ARGV.replace([f1, f2])
  t(:kernel_gets) { [gets, File.basename($FILENAME), readline, gets, gets] }
  t(:rewind_end) { ARGF.rewind }
  t(:readpartial_end) { buf = +"keep"; begin; ARGF.readpartial(1, buf); rescue EOFError; buf; end }
  a = ARGF.class.new(f1)
  t(:seek_noarg) { a.seek }
  t(:seek) { a.seek(3); a.gets }
ensure
  File.unlink(f1, f2)
  Dir.rmdir(dir)
end
