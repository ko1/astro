# IO::Buffer 4.1: slices share the root's lock and validity, resize as views; freeze marks without
# blocking .for's cleanup (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
$VERBOSE = nil
t(:resize_view) { b = IO::Buffer.for("abcdef").dup; s = b.slice(2, 2); [s.resize(4).equal?(s), s.get_string, b.get_string, s.resize(1).get_string] }
t(:resize_zero) { b = IO::Buffer.for("abcdef").dup; s = b.slice(2, 2); s.resize(0); [s.null?, s.empty?, s.valid?, s.resize(4).get_string] }
t(:resize_exceeds) { b = IO::Buffer.for("abcdef").dup; s = b.slice(2, 2); begin; s.resize(5); rescue ArgumentError => e; [e.message, s.get_string]; end }
t(:resize_locked) { b = IO::Buffer.for("abcdef").dup; s = b.slice(2, 2); s.locked { s.resize(4); s.get_string } }
t(:resize_ro) { b = IO::Buffer.for("abcdef"); s = b.slice(2, 2); s.resize(4); [s.readonly?, s.get_string] }
t(:resize_nested) { b = IO::Buffer.for("abcdef").dup; s = b.slice(1, 2).slice(1, 1); s.resize(4); s.get_string }
t(:lock_nested) { b = IO::Buffer.new(4); r = b.locked { b.locked { b.locked? } }; [r, b.locked?] }
t(:lock_shared) do
  b = IO::Buffer.new(4); s = b.slice(0, 2)
  a = b.locked { [s.locked?, s.locked { s.locked? }] }
  c = s.locked { [b.locked?, b.locked { b.locked? }] }
  [a, c, b.locked?, s.locked?]
end
t(:lock_string_slice) { b = IO::Buffer.for("test"); s = b.slice(0, 2); s.locked { begin; b.free; rescue IO::Buffer::LockedError => e; e.class; end } }
t(:lock_invalid) { b = IO::Buffer.new(4); s = b.slice; b.free; y = false; begin; s.locked { y = true }; rescue IO::Buffer::InvalidatedError => e; [e.class, y, s.locked?]; end }
t(:valid_empty) { b = IO::Buffer.new(0); s = b.slice(0, 0); r = [s.valid?]; b.resize(1); r << s.valid?; b.resize(0); r << s.valid? << s.get_string }
t(:valid_string_freed) { b = IO::Buffer.for("alive"); s = b.slice(0, 2); r = [s.valid?]; b.free; r << s.valid? }
t(:for_invalidates) { sl = nil; IO::Buffer.for(+"string") { |b| sl = b.slice(0, 2) }; [sl.valid?, (sl.get_string rescue $!.class)] }
t(:freeze) do
  b = IO::Buffer.new(4).freeze
  [b.frozen?, (b.free rescue $!.class), (b.resize(8) rescue $!.class), b.set_string("x"), b.get_string]
end
t(:for_freeze) { s = +"test"; b = IO::Buffer.for(s, &:freeze); [b.null?, b.frozen?, s << "!"] }
