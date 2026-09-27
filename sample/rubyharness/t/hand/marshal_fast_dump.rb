s = "shared"
a = [1, 2]; a << a
h = Hash.new(5); h[:k] = "v"
vals = [nil, true, false, 0, 1, -1, 122, 123, -123, -124, 255, 256, -256, 65535, 2**30 - 1, 2**30, -2**30, -2**30 - 1, 2**40, -2**40, 2**62 - 1,
        1.0, -1.5, 0.1, 1e20, 1e-5, 123456.789, 1.0/3, -0.0, 0.0, Float::INFINITY, -Float::INFINITY, 1e100, 2.5e-300, 100.0, 1e15, 1e16,
        "", "abc", "日本語", "a".b, "\xff".b, "x".encode("US-ASCII"), :sym, :"with space", [:a, :a, :b],
        [s, s], {a: 1, "b" => [2, 3]}, a, h, {1 => {2 => {3 => [4, [5]]}}}, [[]] * 3, {nil => nil}, [1.5, 1.5]]
vals.each_with_index do |v, i|
  d = Marshal.dump(v)
  print i, " ", d.bytes.map { "%02x" % _1 }.join, "\n"
end
