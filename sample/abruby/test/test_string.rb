require_relative 'test_helper'

class TestString < AbRubyTest
  def test_concat = assert_eval('"hello" + " world"', "hello world")
  def test_repeat = assert_eval('"ab" * 3', "ababab")
  def test_eq_true = assert_eval('"abc" == "abc"', true)
  def test_eq_false = assert_eval('"abc" == "xyz"', false)
  def test_neq = assert_eval('"a" != "b"', true)
  def test_lt = assert_eval('"abc" < "xyz"', true)
  def test_gt = assert_eval('"xyz" > "abc"', true)
  def test_le = assert_eval('"abc" <= "abc"', true)
  def test_ge = assert_eval('"abc" >= "abc"', true)
  def test_length = assert_eval('"hello".length', 5)
  def test_size = assert_eval('"hello".size', 5)
  def test_empty_no = assert_eval('"x".empty?', false)
  def test_empty_yes = assert_eval('"".empty?', true)
  def test_upcase = assert_eval('"hello".upcase', "HELLO")
  def test_downcase = assert_eval('"HELLO".downcase', "hello")
  def test_reverse = assert_eval('"hello".reverse', "olleh")
  def test_include_yes = assert_eval('"hello".include?("ell")', true)
  def test_include_no = assert_eval('"hello".include?("xyz")', false)
  def test_to_s = assert_eval('"hello".to_s', "hello")
  def test_to_i = assert_eval('"42".to_i', 42)
  def test_class = assert_eval('"x".class', "String")

  # interpolation
  def test_interp_basic = assert_eval('"hello #{42}"', "hello 42")
  def test_interp_var = assert_eval('a = "world"; "hello #{a}"', "hello world")
  def test_interp_expr = assert_eval('"#{1 + 2} things"', "3 things")
  def test_interp_multi = assert_eval('"#{1}-#{2}-#{3}"', "1-2-3")
  def test_interp_empty = assert_eval('"hello#{""} world"', "hello world")

  # chained
  def test_chain_upcase_reverse = assert_eval('"hello".upcase.reverse', "OLLEH")
  def test_chain_concat_length = assert_eval('("ab" + "cd").length', 4)

  # variables
  def test_var_concat = assert_eval('a = "foo"; b = "bar"; a + b', "foobar")
  def test_var_compound = assert_eval('a = "x"; a += "y"; a += "z"; a', "xyz")
end
