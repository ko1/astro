require_relative "../../test_helper"

# Kernel#caller — CRuby-compatible behavior with start/length args.

def deep_3
  caller
end
def deep_2
  deep_3                    # line 9
end
def deep_1
  deep_2                    # line 12
end

def test_caller_returns_array_of_strings
  c = deep_1
  assert(c.is_a?(Array), "caller returns Array")
  assert(c.all? { |s| s.is_a?(String) }, "all entries strings")
end

def test_caller_chain_walks_up
  c = deep_1
  # caller skips deep_3 itself (its own frame).  So index 0 is deep_2.
  assert(c[0].include?("`deep_2'"), "first entry: #{c[0]}")
  assert(c[1].include?("`deep_1'"), "second entry: #{c[1]}")
end

def test_caller_lines_are_callsites
  c = deep_1
  # Line 9 is where deep_2 calls deep_3.  Line 12 is where deep_1
  # calls deep_2.
  assert(c[0].include?(":9:"),  "expected :9: got #{c[0]}")
  assert(c[1].include?(":12:"), "expected :12: got #{c[1]}")
end

def returns_caller_with_start(n)
  caller(n)
end

def test_caller_with_start
  c0 = returns_caller_with_start(0)
  c1 = returns_caller_with_start(1)
  c2 = returns_caller_with_start(2)
  # caller(0) includes one extra frame (returns_caller_with_start itself);
  # caller(1) is the default; caller(2) drops one more.
  assert(c0.size > c1.size, "caller(0) longer than caller(1)")
  assert(c1.size > c2.size, "caller(1) longer than caller(2)")
end

def returns_caller_with_start_len(n, m)
  caller(n, m)
end

def test_caller_with_start_and_length
  c = returns_caller_with_start_len(1, 2)
  assert_equal 2, c.size
end

def test_caller_with_range
  def returns_caller_range(r)
    caller(r)
  end
  c = returns_caller_range(0..1)
  assert_equal 2, c.size
end

def test_caller_out_of_range_returns_nil
  def caller_huge_start
    caller(100000)
  end
  r = caller_huge_start
  assert_equal nil, r
end

def test_caller_at_top_level
  c = caller
  assert(c.is_a?(Array), "caller at top works")
end

TESTS = %i[
  test_caller_returns_array_of_strings
  test_caller_chain_walks_up
  test_caller_lines_are_callsites
  test_caller_with_start
  test_caller_with_start_and_length
  test_caller_with_range
  test_caller_out_of_range_returns_nil
  test_caller_at_top_level
]
TESTS.each {|t| run_test(t) }
report "Caller"
