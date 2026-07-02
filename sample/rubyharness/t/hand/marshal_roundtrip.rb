# Marshal round-trip (deep copy) for the common types. vs ruby.
tests = [
  {a: 1, b: [1, 2, 3], c: "str"},
  [1, "two", :three, 4.0, nil, true, false],
  0, 122, 123, -123, 1_000_000, -1_000_000, 2**40, -(2**40), 2**100,
  3.14159, -2.5, 0.0, 100.0,
  "hello world", "", :symbol,
  [[1, 2], [3, 4]], {nested: {deep: [1, {x: "y"}]}},
  {1 => "one", 2 => "two", sym: [nil, true]},
]
tests.each { |t| p(Marshal.load(Marshal.dump(t)) == t) }
orig = [1, [2, 3]]
copy = Marshal.load(Marshal.dump(orig))
copy[1] << 4
p orig
p copy
p Marshal.load(Marshal.dump("independent")).equal?("independent")
