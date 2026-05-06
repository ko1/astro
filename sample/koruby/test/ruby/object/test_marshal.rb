require_relative "../../test_helper"

# Marshal — round-trip + CRuby wire compat for the basic types.

def round_trip(v)
  Marshal.load(Marshal.dump(v))
end

def test_primitives
  assert_equal nil,   round_trip(nil)
  assert_equal true,  round_trip(true)
  assert_equal false, round_trip(false)
end

def test_small_integers
  [0, 1, -1, 5, -5, 100, -100, 122, -123, 123, -124].each do |n|
    assert_equal n, round_trip(n), "round-trip #{n}"
  end
end

def test_larger_integers
  [256, -256, 65535, -65536, 1_000_000, -1_000_000].each do |n|
    assert_equal n, round_trip(n)
  end
end

def test_bignum
  big = 2 ** 64
  assert_equal big, round_trip(big)
  assert_equal -big, round_trip(-big)
end

def test_floats
  [0.0, 1.5, -3.14, 1.0e20, -1.0e-10].each do |f|
    assert_equal f, round_trip(f)
  end
  inf = round_trip(Float::INFINITY)
  assert_equal Float::INFINITY, inf
end

def test_strings
  ["", "hello", "a\nb\tc", "\0\1\2", "x" * 100].each do |s|
    assert_equal s, round_trip(s)
  end
end

def test_symbols
  [:a, :hello_world, :"with space"].each do |sym|
    assert_equal sym, round_trip(sym)
  end
end

def test_arrays
  [[], [1], [1, "two", :three, nil, true],
   [[1, 2], [3, 4]],
   [[[[[42]]]]]].each do |a|
    assert_equal a, round_trip(a)
  end
end

def test_hashes
  [{}, {a: 1}, {"x" => 1, "y" => 2}, {nested: {arr: [1, 2, 3]}}].each do |h|
    assert_equal h, round_trip(h)
  end
end

def test_dump_starts_with_version_header
  d = Marshal.dump(0)
  assert_equal 4, d[0].bytes.first
  assert_equal 8, d[1].bytes.first
end

def test_load_rejects_old_version
  raised = false
  begin
    Marshal.load("\x00\x00" + Marshal.dump(0)[2..-1])
  rescue TypeError
    raised = true
  end
  assert raised
end

def test_symbol_link_dedup_in_dump
  # Repeated symbols should compress via symbol link table — use a
  # long name so the dedup payload (1+1 bytes per repeat) clearly
  # beats fresh emission (1+1+name bytes).
  long = :very_long_symbol_name_that_dedupes
  with_link  = Marshal.dump([long, long, long, long])
  no_dedup_3 = Marshal.dump([:a, :b, :c, :d])
  assert(with_link.size < no_dedup_3.size + long.to_s.size,
         "expected dedup: #{with_link.size} smaller")
  # And direct: symbol referenced 4 times shouldn't grow with each ref.
  ref1 = Marshal.dump([long])
  ref4 = Marshal.dump([long, long, long, long])
  delta = ref4.size - ref1.size
  assert(delta <= 6, "extra refs cost too much: #{delta}")
end

def test_round_trip_with_repeated_symbols
  data = {kinds: [:user, :admin, :user, :admin], main: :user}
  assert_equal data, Marshal.load(Marshal.dump(data))
end

def test_round_trip_complex_nested_with_dedup
  data = {
    a: {x: [:tag, :tag, :tag], y: :tag},
    b: [:tag, {z: :tag}],
    c: :tag,
  }
  assert_equal data, Marshal.load(Marshal.dump(data))
end

TESTS = %i[
  test_primitives test_small_integers test_larger_integers test_bignum
  test_floats test_strings test_symbols test_arrays test_hashes
  test_dump_starts_with_version_header test_load_rejects_old_version
  test_symbol_link_dedup_in_dump
  test_round_trip_with_repeated_symbols
  test_round_trip_complex_nested_with_dedup
]
TESTS.each {|t| run_test(t) }
report "Marshal"
