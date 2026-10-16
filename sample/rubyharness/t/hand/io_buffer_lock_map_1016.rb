# IO::Buffer.for { } locks its String ("can't modify string; temporarily locked", not frozen?), the buffer
# itself still writes it; a SHARED IO::Buffer.map sees another process's writes (2026-10-16)
$VERBOSE = nil
require "tmpdir"
s = +"fur string"
r = IO::Buffer.for(s) do |b|
  e = begin; s[0] = "t"; rescue => x; x; end
  b.set_string("ghost")
  [e.class, e.message, s.frozen?, (begin; s << "x"; rescue => x; x.message; end), s.dup.frozen?, s.clone.frozen?, b.get_string, s]
end
p r
s[1] = "u"; p [s, s.frozen?]
f = "lit".freeze
IO::Buffer.for(f) { p f.frozen? }
p f.frozen?
s2 = +"abc"
p(begin; IO::Buffer.for(s2) { s2.freeze }; rescue => e; [e.class, e.message]; end)
p [s2.frozen?, String.instance_method(:freeze).owner]
Dir.mktmpdir do |d|
  File.open(File.join(d, "m"), "w+") do |file|
    file << "I'm private"; file.rewind
    buf = IO::Buffer.map(file)
    pid = fork { buf.set_string("I'm shared!"); exit!(0) }
    Process.wait(pid)
    p [buf.get_string, file.read]
    priv = IO::Buffer.map(file, nil, 0, IO::Buffer::PRIVATE)
    priv.set_string("private ok!")
    file.rewind
    p [priv.get_string, file.read]
  end
end
