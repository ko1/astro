require_relative "../../test_helper"

# Object#inspect / to_s for various builtins.

def test_inspect_nil
  assert_equal "nil", nil.inspect
end

def test_inspect_true_false
  assert_equal "true",  true.inspect
  assert_equal "false", false.inspect
end

def test_inspect_integer
  assert_equal "42", 42.inspect
  assert_equal "-1", (-1).inspect
end

def test_inspect_string
  assert_equal "\"hello\"", "hello".inspect
  assert_equal '""',         "".inspect
end

def test_inspect_array
  assert_equal "[]",         [].inspect
  assert_equal "[1, 2, 3]",  [1, 2, 3].inspect
  assert_equal "[\"a\"]",    ["a"].inspect
end

def test_inspect_hash
  assert_equal "{}",                {}.inspect
  assert_equal "{:a=>1}",          ({a: 1}.inspect)
end

def test_inspect_symbol
  assert_equal ":foo", :foo.inspect
end

def test_inspect_nested
  assert_equal "[1, [2, 3], {:a=>1}]", [1, [2, 3], {a: 1}].inspect
end

def test_inspect_range
  assert_equal "1..3",  (1..3).inspect
  assert_equal "1...3", (1...3).inspect
end

# ---------- to_s ----------

def test_to_s_integer
  assert_equal "42", 42.to_s
  assert_equal "-7", (-7).to_s
end

def test_to_s_float
  assert_equal "1.5", 1.5.to_s
end

def test_to_s_array
  assert_equal "[1, 2]",      [1, 2].to_s
end

# ---------- custom class inspect default ----------

class CustomInspect
  def initialize
    @x = 1
    @y = "hi"
  end
end

def test_custom_class_default_inspect
  s = CustomInspect.new.inspect
  # CRuby default: "#<CustomInspect:0x... @x=1, @y=\"hi\">"
  # koruby may format differently; just check it has the class name.
  assert s.include?("CustomInspect"), "got #{s.inspect}"
end

TESTS = [
  :test_inspect_nil,
  :test_inspect_true_false,
  :test_inspect_integer,
  :test_inspect_string,
  :test_inspect_array,
  :test_inspect_hash,
  :test_inspect_symbol,
  :test_inspect_nested,
  :test_inspect_range,
  :test_to_s_integer,
  :test_to_s_float,
  :test_to_s_array,
  :test_custom_class_default_inspect,
]
TESTS.each { |t| run_test(t) }
report "Inspect"
