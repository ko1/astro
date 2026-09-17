# Ruby 4.1 IO::Buffer changes: read/write/pread/pwrite take (offset, length)
# (they used to be (length, offset)) and an explicit length of 0 means zero
# bytes; IO::Buffer.new validates its flags instead of raising AllocationError.
require 'tmpdir'
def t(tag); r = yield; puts "#{tag}: => #{r.inspect}"; rescue => e; puts "#{tag}: #{e.class}: #{e.message}"; end
B = IO::Buffer
Dir.mktmpdir do |d|
  path = File.join(d, "b.txt")
  mk = lambda { f = File.open(path, "wb+"); b = B.new(5); b.set_string("Hello"); [f, b] }
  t("write all")  { f, b = mk.(); n = b.write(f); f.rewind; [n, f.read] }
  t("write 1,4")  { f, b = mk.(); n = b.write(f, 1, 4); f.rewind; [n, f.read] }
  t("write 0,0")  { f, b = mk.(); n = b.write(f, 0, 0); f.rewind; [n, f.read] }
  t("write 1,0")  { f, b = mk.(); n = b.write(f, 1, 0); f.rewind; [n, f.read] }
  File.write(path, "World")
  t("read all")   { f = File.open(path, "rb"); b = B.new(5); n = b.read(f); [n, b.get_string] }
  t("read 1,4")   { f = File.open(path, "rb"); b = B.new(5); b.set_string("....."); n = b.read(f, 1, 4); [n, b.get_string] }
  t("pread")      { f = File.open(path, "rb"); b = B.new(5); b.set_string("....."); n = b.pread(f, 0, 0, 2); [n, b.get_string] }
end
t("shared->mapped")  { b = B.new(10, B::SHARED);  r = [b.mapped?, b.shared?]; b.free; r }
t("private->mapped") { b = B.new(10, B::PRIVATE); r = [b.mapped?, b.private?]; b.free; r }
t("infer small")     { b = B.new(10, B::READONLY); r = [b.internal?, b.readonly?]; b.free; r }
t("infer big")       { b = B.new(B::PAGE_SIZE, 0); r = b.mapped?; b.free; r }
t("size 0 flags")    { b = B.new(0, 0xffff); [b.null?, b.empty?, b.internal?, b.mapped?, b.valid?] }
t("INTERNAL|MAPPED") { B.new(10, B::INTERNAL | B::MAPPED) }
t("EXTERNAL")        { B.new(10, B::INTERNAL | B::EXTERNAL) }
t("INTERNAL|SHARED") { B.new(10, B::INTERNAL | B::SHARED) }
t("SHARED|PRIVATE")  { B.new(10, B::SHARED | B::PRIVATE) }
t("negative flags")  { B.new(10, -1) }
t("float flags")     { B.new(10, 0.0) }
t("MAP_ALIGNMENT")   { B::MAP_ALIGNMENT }
