require_relative 'test_helper'

class TestHash < AbRubyTest
  def test_empty = assert_eval("{}", {})
  def test_string_keys = assert_eval('{"a" => 1, "b" => 2}', {"a" => 1, "b" => 2})
  def test_int_keys = assert_eval("{1 => 10, 2 => 20}", {1 => 10, 2 => 20})
  def test_symbol_keys = assert_eval("{a: 1, b: 2}", {"a" => 1, "b" => 2})
  def test_mixed_values = assert_eval('{"x" => 1, "y" => "hello", "z" => true}', {"x" => 1, "y" => "hello", "z" => true})

  def test_get_string = assert_eval('{"a" => 42}["a"]', 42)
  def test_get_int = assert_eval("{1 => 99}[1]", 99)
  def test_get_missing = assert_eval('{"a" => 1}["b"]', nil)
  def test_set = assert_eval('h = {}; h["x"] = 10; h["x"]', 10)
  def test_overwrite = assert_eval('h = {"a" => 1}; h["a"] = 2; h["a"]', 2)
  def test_set_returns = assert_eval('h = {}; h["k"] = 42', 42)

  def test_length = assert_eval('{"a" => 1, "b" => 2}.length', 2)
  def test_size = assert_eval('{"a" => 1}.size', 1)
  def test_empty_true = assert_eval("{}.empty?", true)
  def test_empty_false = assert_eval('{"a" => 1}.empty?', false)
  def test_has_key_true = assert_eval('{"a" => 1}.has_key?("a")', true)
  def test_has_key_false = assert_eval('{"a" => 1}.has_key?("b")', false)
  def test_key_p = assert_eval('{"x" => 1}.key?("x")', true)
  def test_keys = assert_eval('{"a" => 1}.keys', ["a"])
  def test_values = assert_eval('{"a" => 1}.values', [1])
  def test_class = assert_eval('{}.class', "Hash")
  def test_nil_p = assert_eval('{}.nil?', false)

  def test_as_method_arg = assert_eval('def get(h); h["x"]; end; get({"x" => 42})', 42)
  def test_as_ivar = assert_eval(
    'class Config; def initialize; @data = {}; end; def set(k, v); @data[k] = v; end; ' \
    'def get(k); @data[k]; end; end; c = Config.new; c.set("port", 8080); c.get("port")', 8080)
  def test_array_value = assert_eval('{"nums" => [1, 2, 3]}["nums"]', [1, 2, 3])
  def test_nested = assert_eval('{"a" => {"b" => 42}}["a"]["b"]', 42)

  # GC pressure
  def test_gc_many = assert_eval(<<~'RUBY', 500)
    i = 0
    while i < 500
      h = {"key" => i, "val" => i + 1}
      i += 1
    end
    i
  RUBY

  def test_gc_growing = assert_eval(<<~'RUBY', 200)
    h = {}
    i = 0
    while i < 200
      h[i] = i * 2
      i += 1
    end
    h.length
  RUBY

  def test_gc_string_hash = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      h = {"a" => "x", "b" => "y", "c" => "z"}
      i += 1
    end
    i
  RUBY

  def test_gc_hash_array = assert_eval(<<~'RUBY', 50)
    i = 0
    while i < 50
      h = {"items" => [1, 2, 3], "name" => "test"}
      a = h["items"]
      i += 1
    end
    i
  RUBY
end
