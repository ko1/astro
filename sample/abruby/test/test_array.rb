require_relative 'test_helper'

class TestArray < AbRubyTest
  # literal
  def test_empty = assert_eval("[]", [])
  def test_single = assert_eval("[1]", [1])
  def test_multi = assert_eval("[1, 2, 3]", [1, 2, 3])
  def test_mixed = assert_eval('[1, "hello", true, nil]', [1, "hello", true, nil])
  def test_nested = assert_eval("[[1, 2], [3, 4]]", [[1, 2], [3, 4]])
  def test_in_var = assert_eval("a = [1, 2]; a", [1, 2])

  # []
  def test_index_0 = assert_eval("[10, 20, 30][0]", 10)
  def test_index_1 = assert_eval("[10, 20, 30][1]", 20)
  def test_index_neg = assert_eval("[10, 20, 30][-1]", 30)
  def test_index_out = assert_eval("[1, 2][5]", nil)

  # []=
  def test_set = assert_eval("a = [1, 2, 3]; a[1] = 99; a", [1, 99, 3])
  def test_set_returns = assert_eval("a = [1]; a[0] = 42", 42)

  # push / pop
  def test_push = assert_eval("a = []; a.push(1); a.push(2); a", [1, 2])
  def test_pop = assert_eval("a = [1, 2, 3]; a.pop; a", [1, 2])
  def test_pop_returns = assert_eval("[1, 2, 3].pop", 3)
  def test_pop_empty = assert_eval("[].pop", nil)

  # length / size / empty?
  def test_length = assert_eval("[1, 2, 3].length", 3)
  def test_size = assert_eval("[1, 2].size", 2)
  def test_length_empty = assert_eval("[].length", 0)
  def test_empty_true = assert_eval("[].empty?", true)
  def test_empty_false = assert_eval("[1].empty?", false)

  # first / last
  def test_first = assert_eval("[10, 20, 30].first", 10)
  def test_last = assert_eval("[10, 20, 30].last", 30)
  def test_first_empty = assert_eval("[].first", nil)
  def test_last_empty = assert_eval("[].last", nil)

  # +
  def test_concat = assert_eval("[1, 2] + [3, 4]", [1, 2, 3, 4])
  def test_concat_empty = assert_eval("[] + [1]", [1])

  # include?
  def test_include_yes = assert_eval("[1, 2, 3].include?(2)", true)
  def test_include_no = assert_eval("[1, 2, 3].include?(5)", false)

  # class
  def test_class = assert_eval("[].class", "Array")
  def test_nil_p = assert_eval("[].nil?", false)

  # complex
  def test_strings = assert_eval('["a", "b", "c"]', ["a", "b", "c"])
  def test_modify_and_read = assert_eval("a = [0, 0, 0]; a[0] = 1; a[1] = 2; a[2] = 3; a[0] + a[1] + a[2]", 6)
  def test_as_method_arg = assert_eval("def sum(arr); arr[0] + arr[1]; end; sum([10, 20, 30])", 30)
  def test_as_ivar = assert_eval(
    "class Bag; def initialize; @items = []; end; def add(x); @items.push(x); end; " \
    "def count; @items.length; end; end; b = Bag.new; b.add(1); b.add(2); b.add(3); b.count", 3)

  # GC pressure
  def test_gc_many_arrays = assert_eval(<<~'RUBY', 1000)
    i = 0
    while i < 1000
      a = [i, i + 1, i + 2]
      i += 1
    end
    i
  RUBY

  def test_gc_growing = assert_eval(<<~'RUBY', 500)
    a = []
    i = 0
    while i < 500
      a.push(i)
      i += 1
    end
    a.length
  RUBY

  def test_gc_string_arrays = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      a = ["hello", "world", "foo", "bar"]
      i += 1
    end
    i
  RUBY

  def test_gc_nested = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      a = [[1, 2], [3, 4], [5, 6]]
      i += 1
    end
    i
  RUBY

  def test_gc_concat_loop = assert_eval(<<~'RUBY', 10)
    a = []
    i = 0
    while i < 10
      a = a + [i]
      i += 1
    end
    a.length
  RUBY
end
