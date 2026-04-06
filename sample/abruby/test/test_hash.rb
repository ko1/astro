require_relative 'test_helper'

# literal
assert_eval "empty hash", "{}", {}
assert_eval "string keys", '{"a" => 1, "b" => 2}', {"a" => 1, "b" => 2}
assert_eval "int keys", "{1 => 10, 2 => 20}", {1 => 10, 2 => 20}
assert_eval "symbol keys", "{a: 1, b: 2}", {"a" => 1, "b" => 2}  # symbols become strings
assert_eval "mixed values", '{"x" => 1, "y" => "hello", "z" => true}', {"x" => 1, "y" => "hello", "z" => true}

# []
assert_eval "get string key", '{"a" => 42}["a"]', 42
assert_eval "get int key", "{1 => 99}[1]", 99
assert_eval "get missing key", '{"a" => 1}["b"]', nil

# []=
assert_eval "set key", 'h = {}; h["x"] = 10; h["x"]', 10
assert_eval "overwrite key", 'h = {"a" => 1}; h["a"] = 2; h["a"]', 2
assert_eval "set returns value", 'h = {}; h["k"] = 42', 42

# length / size / empty?
assert_eval "length", '{"a" => 1, "b" => 2}.length', 2
assert_eval "size", '{"a" => 1}.size', 1
assert_eval "empty? true", "{}.empty?", true
assert_eval "empty? false", '{"a" => 1}.empty?', false

# has_key? / key?
assert_eval "has_key? true", '{"a" => 1}.has_key?("a")', true
assert_eval "has_key? false", '{"a" => 1}.has_key?("b")', false
assert_eval "key?", '{"x" => 1}.key?("x")', true

# keys / values
assert_eval "keys", '{"a" => 1}.keys', ["a"]
assert_eval "values", '{"a" => 1}.values', [1]

# class / nil?
assert_eval "class", '{}.class', "Hash"
assert_eval "nil?", '{}.nil?', false

# complex
assert_eval "hash as method arg",
  'def get_val(h); h["x"]; end; get_val({"x" => 42})', 42
assert_eval "hash as ivar",
  'class Config; def initialize; @data = {}; end; def set(k, v); @data[k] = v; end; def get(k); @data[k]; end; end; c = Config.new; c.set("port", 8080); c.get("port")', 8080
assert_eval "hash with array value",
  '{"nums" => [1, 2, 3]}["nums"]', [1, 2, 3]
assert_eval "nested hash",
  '{"a" => {"b" => 42}}["a"]["b"]', 42

# GC pressure: many hashes
assert_eval "gc pressure: many hashes", <<~'RUBY', 500
  i = 0
  while i < 500
    h = {"key" => i, "val" => i + 1}
    i += 1
  end
  i
RUBY

# GC pressure: growing hash
assert_eval "gc pressure: growing hash", <<~'RUBY', 200
  h = {}
  i = 0
  while i < 200
    h[i] = i * 2
    i += 1
  end
  h.length
RUBY

# GC pressure: hash with string keys and values
assert_eval "gc pressure: string hash", <<~'RUBY', 100
  i = 0
  while i < 100
    h = {"a" => "x", "b" => "y", "c" => "z"}
    i += 1
  end
  i
RUBY

# GC pressure: hash + array interaction
assert_eval "gc pressure: hash+array", <<~'RUBY', 50
  i = 0
  while i < 50
    h = {"items" => [1, 2, 3], "name" => "test"}
    a = h["items"]
    i += 1
  end
  i
RUBY
