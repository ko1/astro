require_relative 'test_helper'

# literal
assert_eval "empty array", "[]", []
assert_eval "single element", "[1]", [1]
assert_eval "multi element", "[1, 2, 3]", [1, 2, 3]
assert_eval "mixed types", '[1, "hello", true, nil]', [1, "hello", true, nil]
assert_eval "nested array", "[[1, 2], [3, 4]]", [[1, 2], [3, 4]]
assert_eval "array in var", "a = [1, 2]; a", [1, 2]

# []
assert_eval "index 0", "[10, 20, 30][0]", 10
assert_eval "index 1", "[10, 20, 30][1]", 20
assert_eval "index 2", "[10, 20, 30][2]", 30
assert_eval "index -1", "[10, 20, 30][-1]", 30
assert_eval "index out of range", "[1, 2][5]", nil

# []=
assert_eval "set index", "a = [1, 2, 3]; a[1] = 99; a", [1, 99, 3]
assert_eval "set returns value", "a = [1]; a[0] = 42", 42

# push / pop
assert_eval "push", "a = []; a.push(1); a.push(2); a", [1, 2]
assert_eval "pop", "a = [1, 2, 3]; a.pop; a", [1, 2]
assert_eval "pop returns value", "[1, 2, 3].pop", 3
assert_eval "pop empty", "[].pop", nil

# length / size / empty?
assert_eval "length", "[1, 2, 3].length", 3
assert_eval "size", "[1, 2].size", 2
assert_eval "length empty", "[].length", 0
assert_eval "empty? true", "[].empty?", true
assert_eval "empty? false", "[1].empty?", false

# first / last
assert_eval "first", "[10, 20, 30].first", 10
assert_eval "last", "[10, 20, 30].last", 30
assert_eval "first empty", "[].first", nil
assert_eval "last empty", "[].last", nil

# +
assert_eval "concat", "[1, 2] + [3, 4]", [1, 2, 3, 4]
assert_eval "concat empty", "[] + [1]", [1]
assert_eval "concat both empty", "[] + []", []

# include?
assert_eval "include? true", "[1, 2, 3].include?(2)", true
assert_eval "include? false", "[1, 2, 3].include?(5)", false

# class / nil?
assert_eval "class", "[].class", "Array"
assert_eval "nil?", "[].nil?", false

# complex
assert_eval "array of strings", '["a", "b", "c"]', ["a", "b", "c"]
assert_eval "push chain", "a = []; a.push(1); a.push(2); a.push(3); a.length", 3
assert_eval "modify and read", "a = [0, 0, 0]; a[0] = 1; a[1] = 2; a[2] = 3; a[0] + a[1] + a[2]", 6
assert_eval "array as method arg",
  "def sum_first_two(arr); arr[0] + arr[1]; end; sum_first_two([10, 20, 30])", 30
assert_eval "array as ivar",
  "class Bag; def initialize; @items = []; end; def add(x); @items.push(x); end; def count; @items.length; end; end; b = Bag.new; b.add(1); b.add(2); b.add(3); b.count", 3

# GC pressure: many small arrays
assert_eval "gc pressure: many arrays", <<~'RUBY', 1000
  i = 0
  while i < 1000
    a = [i, i + 1, i + 2]
    i += 1
  end
  i
RUBY

# GC pressure: growing array
assert_eval "gc pressure: growing array", <<~'RUBY', 500
  a = []
  i = 0
  while i < 500
    a.push(i)
    i += 1
  end
  a.length
RUBY

# GC pressure: many string arrays
assert_eval "gc pressure: string arrays", <<~'RUBY', 100
  i = 0
  while i < 100
    a = ["hello", "world", "foo", "bar"]
    i += 1
  end
  i
RUBY

# GC pressure: nested arrays
assert_eval "gc pressure: nested arrays", <<~'RUBY', 100
  i = 0
  while i < 100
    a = [[1, 2], [3, 4], [5, 6]]
    i += 1
  end
  i
RUBY

# GC pressure: array concat loop
assert_eval "gc pressure: concat loop", <<~'RUBY', 10
  a = []
  i = 0
  while i < 10
    a = a + [i]
    i += 1
  end
  a.length
RUBY
