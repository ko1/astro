require_relative 'test_helper'

class TestCase < AbRubyTest
  # basic case/when with integers
  def test_case_int_match = assert_eval('case 2; when 1; :a; when 2; :b; else; :c; end', :b)
  def test_case_int_else = assert_eval('case 99; when 1; :a; when 2; :b; else; :c; end', :c)
  def test_case_no_else = assert_eval('case 99; when 1; :a; end', nil)

  # multiple conditions in when
  def test_case_multi_when = assert_eval('case 3; when 1, 2; :a; when 3, 4; :b; end', :b)
  def test_case_multi_first = assert_eval('case 1; when 1, 2; :a; when 3; :b; end', :a)

  # string matching
  def test_case_string = assert_eval('case "hello"; when "hi"; 1; when "hello"; 2; end', 2)

  # symbol matching
  def test_case_symbol = assert_eval('case :foo; when :bar; 1; when :foo; 2; end', 2)

  # class matching (Module#===)
  def test_case_class = assert_eval('case 42; when String; :s; when Integer; :i; end', :i)
  def test_case_class_string = assert_eval('case "hi"; when Integer; :i; when String; :s; end', :s)

  # range matching (Range#===)
  def test_case_range = assert_eval('case 5; when 1..3; :a; when 4..6; :b; else; :c; end', :b)

  # regexp matching (Regexp#===)
  def test_case_regexp = assert_eval('case "hello"; when /^h/; :yes; else; :no; end', :yes)
  def test_case_regexp_no = assert_eval('case "world"; when /^h/; :yes; else; :no; end', :no)

  # case with expression
  def test_case_expr = assert_eval('x = 3; case x * 2; when 5; :a; when 6; :b; end', :b)

  # first match wins
  def test_case_first_wins = assert_eval('case 1; when 1; :a; when 1; :b; end', :a)
end
