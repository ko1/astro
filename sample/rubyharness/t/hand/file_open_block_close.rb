# File.open with a block closes via the object's #close (honours overrides) and
# returns the block's value; a close error propagates. vs ruby.
f = "/tmp/koruby-fob.txt"
File.write(f, "data")
p(File.open(f, "r") { |io| io.read })
p(File.open(f) { 42 })
class MyFile < File
  def close; $closed = true; super; end
end
$closed = false
MyFile.open(f, "r") { |io| io.read }
p $closed
class BadClose < File
  def close; raise "boom"; end
end
begin; BadClose.open(f) { 1 }; rescue => e; p e.message; end
File.delete(f)
