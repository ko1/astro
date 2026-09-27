# A truncated Marshal stream from an IO: EOFError where CRuby reads a single byte with
# nothing pending (#getbyte), "marshal data too short" where it reads a promised run.
require "stringio"
S2 = Struct.new(:a, :b)
[Marshal.dump([1, 2, 3]), Marshal.dump({foo: [1, "abc"]}), Marshal.dump("hello"), Marshal.dump(300), Marshal.dump(S2.new(1, [2]))].each do |s|
  puts (0...s.bytesize).map { |n| r = (Marshal.load(StringIO.new(s.byteslice(0, n))) rescue $!.class.to_s[0, 3]); "#{n}:#{r}" }.join(" ")
end
