# Short strings keep their bytes in the object: lengths around the inline limit, growing out of it with <<,
# in-place edits, encodings, freezing, GC moving them, and strings used as hash keys
3.times do |r|
  GC.start if r == 1
  [0, 1, 7, 8, 31, 38, 39, 40, 41, 64].each do |n|
    s = "a" * n
    t = s.dup; t << "xyz"; t.upcase!
    u = +"é" * (n / 2); u.force_encoding("BINARY")
    p [n, s.size, t, t.size, u.bytesize, u.encoding, s.frozen?, s.hash == ("a" * n).hash]
  end
  b = +""; 50.times { |i| b << (i % 10).to_s; b.sub!(/0$/, "Z") if i % 13 == 0 }; p b, b.size
  c = "hello world"; c[0, 5] = "HELLO" * 9; p c, c.size; c.slice!(3, 40); p c
  d = "x".freeze; p d.frozen?, d + "y", (d << "z" rescue $!.class)
  h = {}; 20.times { |i| h["k#{i}"] = i.to_s * i }; GC.start; p h["k3"], h["k19"].size, h.keys.last
  e = "abc".encode("UTF-16LE"); p e.bytesize, e.encode("UTF-8")
  f = ["short", "a" * 45].map { |x| x.tr("a", "b").reverse.center(50, "*") }; p f
  g = :sym_to_string.to_s; g << "!"; p g, 12345.to_s * 9, 1.5.to_s, [1, "two", :three].join("-")
end
