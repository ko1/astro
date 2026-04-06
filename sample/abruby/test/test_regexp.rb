require_relative 'test_helper'

class TestRegexp < AbRubyTest
  # literal
  def test_literal = assert_eval('/hello/', /hello/)
  def test_literal_complex = assert_eval('/[a-z]+/', /[a-z]+/)

  # match?
  def test_match_p_yes = assert_eval('/hello/.match?("hello world")', true)
  def test_match_p_no = assert_eval('/hello/.match?("goodbye")', false)
  def test_match_p_pattern = assert_eval('/^[0-9]+$/.match?("12345")', true)
  def test_match_p_fail = assert_eval('/^[0-9]+$/.match?("abc")', false)

  # match
  def test_match_found = assert_eval('/hello/.match("hello")', true)
  def test_match_not_found = assert_eval('/hello/.match("world")', nil)

  # source
  def test_source = assert_eval('/hello/.source', "hello")
  def test_source_complex = assert_eval('/[a-z]+/.source', "[a-z]+")

  # ==
  def test_eq_true = assert_eval('/foo/ == /foo/', true)
  def test_eq_false = assert_eval('/foo/ == /bar/', false)

  # =~
  def test_eqtilde_match = assert_eval('/world/ =~ "hello world"', 6)
  def test_eqtilde_no_match = assert_eval('/xyz/ =~ "hello"', nil)

  # class
  def test_class = assert_eval('/foo/.class', "Regexp")

  # in variable
  def test_var = assert_eval('r = /[0-9]+/; r.match?("123")', true)

  # as method arg
  def test_method_arg = assert_eval('def check(re, s); re.match?(s); end; check(/hello/, "hello world")', true)
end
