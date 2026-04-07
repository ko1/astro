require_relative 'test_helper'

class TestCase < AbRubyTest
  # basic case/when with integers
  def test_case_int_match = assert_eval('case 2; when 1; :a; when 2; :b; else; :c; end', :b)
  def test_case_int_first = assert_eval('case 1; when 1; :a; when 2; :b; else; :c; end', :a)
  def test_case_int_else = assert_eval('case 99; when 1; :a; when 2; :b; else; :c; end', :c)
  def test_case_no_else = assert_eval('case 99; when 1; :a; end', nil)

  # else returns a value
  def test_else_value_int = assert_eval('case 0; when 1; :a; else; 42; end', 42)
  def test_else_value_str = assert_eval('case 0; when 1; :a; else; "fallback"; end', "fallback")


  # multiple conditions in when
  def test_case_multi_when = assert_eval('case 3; when 1, 2; :a; when 3, 4; :b; end', :b)
  def test_case_multi_first = assert_eval('case 1; when 1, 2; :a; when 3; :b; end', :a)
  def test_case_multi_second = assert_eval('case 2; when 1, 2; :a; when 3; :b; end', :a)
  def test_case_multi_miss = assert_eval('case 5; when 1, 2; :a; when 3, 4; :b; else; :c; end', :c)

  # string matching
  def test_case_string_match = assert_eval('case "hello"; when "hi"; 1; when "hello"; 2; end', 2)
  def test_case_string_else = assert_eval('case "x"; when "a"; 1; when "b"; 2; else; 3; end', 3)

  # symbol matching
  def test_case_symbol = assert_eval('case :foo; when :bar; 1; when :foo; 2; end', 2)

  # nil / true / false matching
  def test_case_nil = assert_eval('case nil; when nil; :yes; else; :no; end', :yes)
  def test_case_true = assert_eval('case true; when true; :yes; else; :no; end', :yes)
  def test_case_false = assert_eval('case false; when false; :yes; when nil; :nil; else; :no; end', :yes)

  # class matching (Module#===)
  def test_case_class_int = assert_eval('case 42; when String; :s; when Integer; :i; end', :i)
  def test_case_class_str = assert_eval('case "hi"; when Integer; :i; when String; :s; end', :s)
  def test_case_class_nil = assert_eval('case nil; when Integer; :i; when String; :s; else; :other; end', :other)
  def test_case_class_user = assert_eval(
    'class TcCW; end; case TcCW.new; when Integer; :i; when TcCW; :cw; end', :cw)

  # range matching (Range#===)
  def test_case_range = assert_eval('case 5; when 1..3; :a; when 4..6; :b; else; :c; end', :b)
  def test_case_range_miss = assert_eval('case 10; when 1..3; :a; when 4..6; :b; else; :c; end', :c)
  def test_case_range_edge = assert_eval('case 3; when 1..3; :yes; else; :no; end', :yes)

  # regexp matching (Regexp#===)
  def test_case_regexp_yes = assert_eval('case "hello"; when /^h/; :yes; else; :no; end', :yes)
  def test_case_regexp_no = assert_eval('case "world"; when /^h/; :yes; else; :no; end', :no)

  # case with expression predicate
  def test_case_expr = assert_eval('x = 3; case x * 2; when 5; :a; when 6; :b; end', :b)

  # first match wins
  def test_case_first_wins = assert_eval('case 1; when 1; :a; when 1; :b; end', :a)

  # when body with multiple statements
  def test_case_multi_stmt = assert_eval('case 1; when 1; x = 10; x + 5; when 2; 0; end', 15)

  # case with many whens
  def test_case_many_whens = assert_eval(
    'case 4; when 1; :a; when 2; :b; when 3; :c; when 4; :d; when 5; :e; end', :d)

  # nested case
  def test_case_nested = assert_eval('
    case 1
    when 1
      case 2
      when 1; :a
      when 2; :b
      end
    when 2; :c
    end
  ', :b)
end
