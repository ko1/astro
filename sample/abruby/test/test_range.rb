require_relative 'test_helper'

class TestRange < AbRubyTest
  # literal
  def test_inclusive = assert_eval("1..10", 1..10)
  def test_exclusive = assert_eval("1...10", 1...10)
  def test_negative = assert_eval("-5..5", -5..5)
  def test_single = assert_eval("1..1", 1..1)

  # first / last / begin / end
  def test_first = assert_eval("(1..10).first", 1)
  def test_last = assert_eval("(1..10).last", 10)
  def test_begin = assert_eval("(1..10).begin", 1)
  def test_end = assert_eval("(1..10).end", 10)

  # exclude_end?
  def test_exclude_end_inclusive = assert_eval("(1..10).exclude_end?", false)
  def test_exclude_end_exclusive = assert_eval("(1...10).exclude_end?", true)

  # size
  def test_size_inclusive = assert_eval("(1..10).size", 10)
  def test_size_exclusive = assert_eval("(1...10).size", 9)
  def test_size_empty = assert_eval("(5..3).size", 0)
  def test_size_single = assert_eval("(1..1).size", 1)

  # include?
  def test_include_yes = assert_eval("(1..10).include?(5)", true)
  def test_include_no = assert_eval("(1..10).include?(11)", false)
  def test_include_first = assert_eval("(1..10).include?(1)", true)
  def test_include_last_incl = assert_eval("(1..10).include?(10)", true)
  def test_include_last_excl = assert_eval("(1...10).include?(10)", false)
  def test_include_excl_yes = assert_eval("(1...10).include?(9)", true)

  # to_a
  def test_to_a_small = assert_eval("(1..5).to_a", [1, 2, 3, 4, 5])
  def test_to_a_exclusive = assert_eval("(1...5).to_a", [1, 2, 3, 4])
  def test_to_a_single = assert_eval("(3..3).to_a", [3])

  # ==
  def test_eq_true = assert_eval("(1..10) == (1..10)", true)
  def test_eq_false_end = assert_eval("(1..10) == (1..11)", false)
  def test_eq_false_type = assert_eval("(1..10) == (1...10)", false)

  # inspect / to_s
  def test_inspect = assert_eval("(1..10).inspect", "1..10")
  def test_inspect_excl = assert_eval("(1...10).inspect", "1...10")

  # class
  def test_class = assert_eval("(1..10).class", "Range")

  # in variables
  def test_var = assert_eval("r = 1..5; r.size", 5)

  # as method arg
  def test_method_arg = assert_eval("def sz(r); r.size; end; sz(1..10)", 10)

  # computed range
  def test_computed = assert_eval("a = 3; b = 7; (a..b).size", 5)
end
