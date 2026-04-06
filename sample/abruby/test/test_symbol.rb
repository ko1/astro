require_relative 'test_helper'

class TestSymbol < AbRubyTest
  # literal
  def test_sym_literal = assert_eval(":foo", :foo)
  def test_sym_hello = assert_eval(":hello", :hello)
  def test_sym_with_question = assert_eval(":empty?", :empty?)
  def test_sym_with_bang = assert_eval(":save!", :save!)

  # equality
  def test_eq_true = assert_eval(":foo == :foo", true)
  def test_eq_false = assert_eval(":foo == :bar", false)
  def test_neq_true = assert_eval(":foo != :bar", true)
  def test_neq_false = assert_eval(":foo != :foo", false)

  # methods
  def test_to_s = assert_eval(":hello.to_s", "hello")
  def test_to_sym = assert_eval(":hello.to_sym", :hello)
  def test_inspect = assert_eval(":hello.inspect", ":hello")
  def test_class = assert_eval(":foo.class", "Symbol")

  # as hash key
  def test_hash_symbol_key = assert_eval("{a: 42}[:a]", 42)
  def test_hash_symbol_key_explicit = assert_eval('{:x => 10}[:x]', 10)
  def test_hash_set_symbol = assert_eval('h = {}; h[:k] = 99; h[:k]', 99)

  # in arrays
  def test_sym_in_array = assert_eval("[:a, :b, :c]", [:a, :b, :c])
  def test_array_include_sym = assert_eval("[:a, :b].include?(:b)", true)

  # %i literal
  def test_percent_i = assert_eval("%i(x y z)", [:x, :y, :z])
  def test_percent_i_single = assert_eval("%i(foo)", [:foo])

  # variables
  def test_sym_var = assert_eval("a = :hello; a", :hello)
  def test_sym_eq_var = assert_eval("a = :foo; b = :foo; a == b", true)

  # as method arg
  def test_sym_method_arg = assert_eval("def check(s); s == :ok; end; check(:ok)", true)
end
