require_relative 'test_helper'

# basic string ops
assert_eval "str +", '"hello" + " world"', "hello world"
assert_eval "str *", '"ab" * 3', "ababab"
assert_eval "str ==", '"abc" == "abc"', true
assert_eval "str == false", '"abc" == "xyz"', false
assert_eval "str !=", '"a" != "b"', true
assert_eval "str <", '"abc" < "xyz"', true
assert_eval "str >", '"xyz" > "abc"', true
assert_eval "str <=", '"abc" <= "abc"', true
assert_eval "str >=", '"abc" >= "abc"', true
assert_eval "str length", '"hello".length', 5
assert_eval "str size", '"hello".size', 5
assert_eval "str empty? no", '"x".empty?', false
assert_eval "str empty? yes", '"".empty?', true
assert_eval "str upcase", '"hello".upcase', "HELLO"
assert_eval "str downcase", '"HELLO".downcase', "hello"
assert_eval "str reverse", '"hello".reverse', "olleh"
assert_eval "str include?", '"hello".include?("ell")', true
assert_eval "str include? no", '"hello".include?("xyz")', false
assert_eval "str to_s", '"hello".to_s', "hello"
assert_eval "str to_i", '"42".to_i', 42
assert_eval "str class", '"x".class', "String"

# string interpolation
assert_eval "interpolation basic", '"hello #{42}"', "hello 42"
assert_eval "interpolation var", 'a = "world"; "hello #{a}"', "hello world"
assert_eval "interpolation expr", '"#{1 + 2} things"', "3 things"
assert_eval "interpolation multi", '"#{1}-#{2}-#{3}"', "1-2-3"
assert_eval "interpolation empty", '"hello#{""} world"', "hello world"

# chained string methods
assert_eval "chain upcase reverse", '"hello".upcase.reverse', "OLLEH"
assert_eval "chain + length", '("ab" + "cd").length', 4

# string in variables
assert_eval "str var concat", 'a = "foo"; b = "bar"; a + b', "foobar"
assert_eval "str var +=", 'a = "x"; a += "y"; a += "z"; a', "xyz"
