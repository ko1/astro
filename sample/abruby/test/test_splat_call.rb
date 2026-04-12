require_relative 'test_helper'

class TestSplatCall < AbRubyTest
  # func_call_apply (no receiver)
  def test_func_splat_only = assert_eval(
    "def f(a,b,c); [a,b,c]; end; f(*[1,2,3])", [1,2,3])
  def test_func_splat_prefix = assert_eval(
    "def f(a,b,c); [a,b,c]; end; f(1, *[2,3])", [1,2,3])
  def test_func_splat_suffix = assert_eval(
    "def f(a,b,c); [a,b,c]; end; f(*[1,2], 3)", [1,2,3])
  def test_func_splat_middle = assert_eval(
    "def f(a,b,c,d); [a,b,c,d]; end; f(1, *[2,3], 4)", [1,2,3,4])
  def test_func_double_splat = assert_eval(
    "def f(a,b,c,d); [a,b,c,d]; end; f(*[1,2], *[3,4])", [1,2,3,4])
  def test_func_splat_from_var = assert_eval(
    "def f(a,b,c); [a,b,c]; end; x=[2,3]; f(1, *x)", [1,2,3])
  def test_func_splat_empty_array = assert_eval(
    "def f(a,b); [a,b]; end; f(*[], 1, 2)", [1,2])

  # method_call_apply (explicit receiver)
  def test_method_splat_only = assert_eval(<<~RUBY, [1,2,3])
    class A
      def m(a,b,c); [a,b,c]; end
    end
    A.new.m(*[1,2,3])
  RUBY

  def test_method_splat_mixed = assert_eval(<<~RUBY, [1,2,3,4,5])
    class A
      def m(a,b,c,d,e); [a,b,c,d,e]; end
    end
    A.new.m(1, *[2,3], 4, *[5])
  RUBY

  def test_method_splat_returning_method_result = assert_eval(<<~RUBY, 6)
    class A
      def vals; [1, 2, 3]; end
      def sum(a,b,c); a + b + c; end
    end
    a = A.new
    a.sum(*a.vals)
  RUBY

  # splat where recv and args mix with nested calls
  def test_splat_with_nested_calls = assert_eval(<<~RUBY, 10)
    def pair; [3, 4]; end
    def f(a, b, c); a + b + c; end
    f(3, *pair)
  RUBY

  # splat where self-call via explicit self
  def test_splat_explicit_self = assert_eval(<<~RUBY, [1,2,3])
    class A
      def m(a,b,c); [a,b,c]; end
      def go; self.m(1, *[2,3]); end
    end
    A.new.go
  RUBY

  # larger dynamic count
  def test_splat_large = assert_eval(<<~RUBY, 55)
    def sum(a,b,c,d,e,f,g,h,i,j); a+b+c+d+e+f+g+h+i+j; end
    arr = [1,2,3,4,5,6,7,8,9,10]
    sum(*arr)
  RUBY

  # nested splat calls
  def test_splat_nested = assert_eval(<<~RUBY, [1,2,3,4])
    def f(a,b,c,d); [a,b,c,d]; end
    def g(x,y); [x,y]; end
    f(*g(1,2), *g(3,4))
  RUBY

  # Ruby's splat treats a non-Array as a 1-element array (`[*42] == [42]`).
  # abruby's Array#+ is lenient enough to follow that.
  def test_splat_non_array_wraps = assert_eval(<<~RUBY, 42)
    def f(a); a; end
    f(*42)
  RUBY
end
