require_relative "../../test_helper"

# JSON — minimal parse/generate.

def test_generate_primitives
  assert_equal "null",  JSON.generate(nil)
  assert_equal "true",  JSON.generate(true)
  assert_equal "false", JSON.generate(false)
  assert_equal "42",    JSON.generate(42)
  assert_equal "3.14",  JSON.generate(3.14)
end

def test_generate_strings
  assert_equal '"hello"', JSON.generate("hello")
  assert_equal '"a\nb"',  JSON.generate("a\nb")
  assert_equal '"\""',    JSON.generate("\"")
  assert_equal '"\\\\"',  JSON.generate("\\")
  assert_equal '"\t"',    JSON.generate("\t")
end

def test_generate_array
  assert_equal "[]", JSON.generate([])
  assert_equal "[1,2,3]", JSON.generate([1, 2, 3])
  assert_equal '["a","b"]', JSON.generate(["a", "b"])
  assert_equal '[1,[2,[3]]]', JSON.generate([1, [2, [3]]])
end

def test_generate_hash
  assert_equal "{}", JSON.generate({})
  assert_equal '{"a":1}', JSON.generate({"a" => 1})
  # Symbol keys turn into strings
  assert_equal '{"a":1}', JSON.generate({a: 1})
end

def test_dump_alias
  assert_equal '{"x":1}', JSON.dump({x: 1})
end

def test_parse_primitives
  assert_equal nil,   JSON.parse("null")
  assert_equal true,  JSON.parse("true")
  assert_equal false, JSON.parse("false")
  assert_equal 42,    JSON.parse("42")
  assert_equal 3.14,  JSON.parse("3.14")
end

def test_parse_string
  assert_equal "hello", JSON.parse('"hello"')
  assert_equal "a\nb",  JSON.parse('"a\nb"')
  assert_equal "\"",    JSON.parse('"\""')
  assert_equal "\t",    JSON.parse('"\t"')
end

def test_parse_array
  assert_equal [],         JSON.parse("[]")
  assert_equal [1, 2, 3],  JSON.parse("[1,2,3]")
  assert_equal ["a", "b"], JSON.parse('["a","b"]')
end

def test_parse_hash
  assert_equal({}, JSON.parse("{}"))
  assert_equal({"x" => 1}, JSON.parse('{"x":1}'))
  assert_equal({"a" => 1, "b" => [2, 3]}, JSON.parse('{"a":1,"b":[2,3]}'))
end

def test_parse_with_whitespace
  assert_equal({"a" => 1}, JSON.parse("  { \"a\" : 1 }  "))
end

def test_round_trip
  obj = {"name" => "alice", "age" => 30, "tags" => ["admin", "user"], "meta" => nil}
  assert_equal obj, JSON.parse(JSON.generate(obj))
end

def test_load_alias
  assert_equal [1, 2, 3], JSON.load("[1,2,3]")
end

def test_unicode_escape
  # A = 'A'
  assert_equal "A", JSON.parse('"A"')
end

def test_parse_error_on_bad_input
  raised = false
  begin
    JSON.parse("[1, 2,")
  rescue
    raised = true
  end
  assert raised
end

def test_pretty_generate_basic
  out = JSON.pretty_generate({a: 1, b: [2, 3]})
  assert out.include?("\n")
  assert out.include?("  ")
  # Round-trip back to same data.
  assert_equal({"a" => 1, "b" => [2, 3]}, JSON.parse(out))
end

def test_pretty_generate_empty
  assert_equal "[]", JSON.pretty_generate([])
  assert_equal "{}", JSON.pretty_generate({})
end

def test_pretty_generate_nesting_indent
  out = JSON.pretty_generate({a: {b: {c: 1}}})
  # Innermost c on level-3 indent.
  assert out.include?("      \"c\":")
end

def test_parse_symbolize_names
  h = JSON.parse('{"x":1,"y":[2,3]}', symbolize_names: true)
  assert_equal({x: 1, y: [2, 3]}, h)
end

def test_parse_symbolize_names_nested
  h = JSON.parse('{"a":{"b":{"c":1}}}', symbolize_names: true)
  assert_equal({a: {b: {c: 1}}}, h)
end

def test_load_with_symbolize_names
  h = JSON.load('{"a":1}', symbolize_names: true)
  assert_equal({a: 1}, h)
end

TESTS = %i[
  test_generate_primitives test_generate_strings test_generate_array
  test_generate_hash test_dump_alias
  test_parse_primitives test_parse_string test_parse_array
  test_parse_hash test_parse_with_whitespace test_round_trip
  test_load_alias test_unicode_escape test_parse_error_on_bad_input
  test_pretty_generate_basic test_pretty_generate_empty
  test_pretty_generate_nesting_indent
  test_parse_symbolize_names test_parse_symbolize_names_nested
  test_load_with_symbolize_names
]
TESTS.each {|t| run_test(t) }
report "JSON"
