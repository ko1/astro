require_relative "../../test_helper"

# Array#to_h, Range#to_h, Symbol#to_sym, String#case_bangs

def test_array_to_h_pairs
  assert_equal({a: 1, b: 2}, [[:a, 1], [:b, 2]].to_h)
end

def test_array_to_h_block
  assert_equal({1=>1, 2=>4, 3=>9}, [1, 2, 3].to_h { |i| [i, i*i] })
end

def test_array_to_h_empty
  assert_equal({}, [].to_h)
end

def test_array_to_h_bad_pair_raises
  raised = false
  begin
    [[1, 2], [3]].to_h
  rescue TypeError
    raised = true
  end
  assert_equal true, raised
end

def test_range_to_h_block
  assert_equal({1=>"one", 2=>"two"}, (1..2).to_h { |i| [i, ["", "one", "two"][i]] })
end

def test_symbol_to_sym
  assert_equal :foo, :foo.to_sym
  assert :foo.to_sym.equal?(:foo)
end

def test_symbol_id2name
  assert_equal "bar", :bar.id2name
end

# ---------- String case bangs ----------

def test_upcase_bang
  s = String.new("hello")
  result = s.upcase!
  assert_equal "HELLO", s
  assert_equal "HELLO", result
end

def test_upcase_bang_no_change_returns_nil
  s = String.new("HELLO")
  assert_equal nil, s.upcase!
end

def test_downcase_bang
  s = String.new("HELLO")
  s.downcase!
  assert_equal "hello", s
end

def test_swapcase_bang
  s = String.new("Hello")
  s.swapcase!
  assert_equal "hELLO", s
end

def test_capitalize_bang
  s = String.new("hELLO")
  s.capitalize!
  assert_equal "Hello", s
end

def test_reverse_bang
  s = String.new("abc")
  s.reverse!
  assert_equal "cba", s
end

def test_dup_then_bang_does_not_affect_original
  orig = "hello"
  d = orig.dup
  d.upcase!
  assert_equal "hello", orig
  assert_equal "HELLO", d
end

TESTS = [
  :test_array_to_h_pairs,
  :test_array_to_h_block,
  :test_array_to_h_empty,
  :test_array_to_h_bad_pair_raises,
  :test_range_to_h_block,
  :test_symbol_to_sym,
  :test_symbol_id2name,
  :test_upcase_bang,
  :test_upcase_bang_no_change_returns_nil,
  :test_downcase_bang,
  :test_swapcase_bang,
  :test_capitalize_bang,
  :test_reverse_bang,
  :test_dup_then_bang_does_not_affect_original,
]
TESTS.each { |t| run_test(t) }
report "ToHCaseBang"
