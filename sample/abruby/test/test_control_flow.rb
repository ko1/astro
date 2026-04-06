require_relative 'test_helper'

# node_if
assert_eval "if true", "if true; 1; else; 2; end", 1
assert_eval "if false", "if false; 1; else; 2; end", 2
assert_eval "if nil is falsy", "if nil; 1; else; 2; end", 2
assert_eval "if 0 is truthy", "if 0; 1; else; 2; end", 1
assert_eval "if no else (true)", "if true; 42; end", 42
assert_eval "if no else (false)", "if false; 42; end", nil
assert_eval "if with expr", "a = 5; if a > 3; 1; else; 0; end", 1

# unless
assert_eval "unless true", "unless true; 1; else; 2; end", 2
assert_eval "unless false", "unless false; 1; else; 2; end", 1

# node_while
assert_eval "while basic", "a = 0; i = 0; while i < 5; a += 1; i += 1; end; a", 5
assert_eval "while returns nil", "i = 0; while i < 1; i += 1; end", nil
assert_eval "while false", "a = 0; while false; a += 1; end; a", 0

# node_seq
assert_eval "seq returns last", "1; 2; 3", 3
