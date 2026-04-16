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

  # equality with non-symbol
  def test_eq_nil = assert_eval(":foo == nil", false)
  def test_eq_string = assert_eval(':foo == "foo"', false)
  def test_eq_int = assert_eval(":foo == 1", false)
  def test_neq_nil = assert_eval(":foo != nil", true)

  # methods
  def test_to_s = assert_eval(":hello.to_s", "hello")
  def test_to_sym = assert_eval(":hello.to_sym", :hello)
  def test_inspect = assert_eval(":hello.inspect", ":hello")
  def test_class = assert_eval(":foo.class", "Symbol")

  # String#to_sym / String#intern
  def test_string_to_sym = assert_eval('"foo".to_sym', :foo)
  def test_string_intern = assert_eval('"bar".intern', :bar)
  def test_string_to_sym_eq = assert_eval('"foo".to_sym == :foo', true)
  def test_string_to_sym_inspect = assert_eval('"hello".to_sym.inspect', ":hello")
  def test_string_to_sym_to_s = assert_eval('"world".to_sym.to_s', "world")

  # interpolated symbol literal (InterpolatedSymbolNode → String#to_sym)
  def test_interpolated_sym = assert_eval('x = "bar"; :"foo_#{x}"', :"foo_bar")
  def test_interpolated_sym_eq = assert_eval('x = "b"; :"a_#{x}" == :"a_b"', true)
  def test_interpolated_sym_inspect = assert_eval('x = "hi"; :"@#{x}".inspect', ":@hi")
  def test_interpolated_sym_to_s = assert_eval('x = "v"; :"@#{x}".to_s', "@v")
  def test_interpolated_sym_class = assert_eval('x = "z"; :"#{x}".class', "Symbol")
  def test_interpolated_sym_ivar_style = assert_eval('x = "name"; :"@#{x}" == :"@name"', true)
  def test_interpolated_sym_as_hash_key = assert_eval('x = "k"; h = {}; h[:"#{x}"] = 1; h[:k]', 1)
  def test_interpolated_sym_p = assert_eval('x = "a"; :"#{x}".inspect', ":a")
  def test_interpolated_sym_in_array = assert_eval('x = "q"; [:"#{x}"]', [:q])
  def test_interpolated_sym_nested = assert_eval('a = "x"; b = "y"; :"#{a}_#{b}"', :"x_y")

  # as hash key
  def test_hash_symbol_key = assert_eval("{a: 42}[:a]", 42)
  def test_hash_symbol_key_explicit = assert_eval('{:x => 10}[:x]', 10)
  def test_hash_set_symbol = assert_eval('h = {}; h[:k] = 99; h[:k]', 99)
  def test_hash_sym_key_lookup_from_to_sym = assert_eval('h = {foo: 1}; h["foo".to_sym]', 1)
  def test_hash_each_sym_key = assert_eval('h = {a: 1, b: 2}; r = []; h.each { |k,v| r = r + [k] }; r', [:a, :b])
  def test_hash_each_key_sym = assert_eval('h = {x: 10}; r = nil; h.each_key { |k| r = k }; r', :x)
  def test_hash_keys_sym = assert_eval('{a: 1, b: 2}.keys', [:a, :b])

  # in arrays
  def test_sym_in_array = assert_eval("[:a, :b, :c]", [:a, :b, :c])
  def test_array_include_sym = assert_eval("[:a, :b].include?(:b)", true)
  def test_array_include_sym_false = assert_eval("[:a, :b].include?(:c)", false)
  def test_array_include_to_sym = assert_eval('[:foo, :bar].include?("foo".to_sym)', true)

  # %i literal
  def test_percent_i = assert_eval("%i(x y z)", [:x, :y, :z])
  def test_percent_i_single = assert_eval("%i(foo)", [:foo])

  # variables
  def test_sym_var = assert_eval("a = :hello; a", :hello)
  def test_sym_eq_var = assert_eval("a = :foo; b = :foo; a == b", true)

  # as method arg
  def test_sym_method_arg = assert_eval("def check(s); s == :ok; end; check(:ok)", true)

  # case/when with symbols
  def test_case_when_sym = assert_eval('case :foo; when :foo; 1; when :bar; 2; end', 1)
  def test_case_when_sym_second = assert_eval('case :bar; when :foo; 1; when :bar; 2; end', 2)
  def test_case_when_sym_else = assert_eval('case :baz; when :foo; 1; else; 0; end', 0)

  # symbol in send / respond_to? / method
  def test_send_with_sym = assert_eval('1.send(:to_s)', "1")
  def test_send_with_to_sym = assert_eval('1.send("to_s".to_sym)', "1")
  def test_respond_to_sym = assert_eval('"hi".respond_to?(:length)', true)
  def test_respond_to_sym_false = assert_eval('"hi".respond_to?(:nonexistent)', false)

  # attr_reader / attr_writer with symbols
  def test_attr_reader_sym = assert_eval('class A; attr_reader :x; def initialize; @x = 42; end; end; A.new.x', 42)
  def test_attr_writer_sym = assert_eval('class B; attr_writer :y; attr_reader :y; def initialize; self.y = 7; end; end; B.new.y', 7)
  def test_attr_accessor_sym = assert_eval('class C; attr_accessor :z; end; c = C.new; c.z = 5; c.z', 5)

  # const_get / const_set with symbols
  def test_const_get_sym = assert_eval('class M; X = 10; end; M.const_get(:X)', 10)
  def test_const_set_sym = assert_eval('class N; end; N.const_set(:Y, 20); N.const_get(:Y)', 20)

  # instance_variable_get/set with symbols
  def test_ivar_get_sym = assert_eval('class D; def initialize; @v = 3; end; end; D.new.instance_variable_get(:@v)', 3)
  def test_ivar_set_sym = assert_eval('class E; end; e = E.new; e.instance_variable_set(:@w, 8); e.instance_variable_get(:@w)', 8)

  # interpolated symbol used as ivar name
  def test_interpolated_sym_ivar_set = assert_eval('
    class F; end
    f = F.new
    name = "val"
    f.instance_variable_set(:"@#{name}", 99)
    f.instance_variable_get(:@val)
  ', 99)

  # symbol returned from block / proc
  def test_sym_from_block = assert_eval('[1,2,3].map { |x| :ok }', [:ok, :ok, :ok])
  def test_sym_from_method = assert_eval('def sym; :result; end; sym', :result)

  # symbol as hash value (not key)
  def test_sym_as_hash_value = assert_eval('h = {x: :yes}; h[:x]', :yes)
  def test_sym_as_hash_value_eq = assert_eval('h = {x: :yes}; h[:x] == :yes', true)
end
