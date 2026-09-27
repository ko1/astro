# A truncated Marshal stream from an IO is EOFError (from a String: ArgumentError).
require "stringio"
s = Marshal.dump({foo: [1, "abc"]})
[1, 3, s.bytesize - 1].each do |n|
  t = s.byteslice(0, n)
  r = [(Marshal.load(t) rescue $!.class), (Marshal.load(StringIO.new(t)) rescue [$!.class, $!.message])]
  rd, w = IO.pipe; w.write(t); w.close; r << (Marshal.load(rd) rescue $!.class)
  p [n, r]
end
io = StringIO.new(Marshal.dump(1) + Marshal.dump([2])); p Marshal.load(io), Marshal.load(io), (Marshal.load(io) rescue $!.class)
