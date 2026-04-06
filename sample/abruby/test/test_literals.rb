require_relative 'test_helper'

# node_num
assert_eval "num 0", "0", 0
assert_eval "num positive", "42", 42
assert_eval "num negative", "-1", -1

# node_true / node_false / node_nil
assert_eval "true", "true", true
assert_eval "false", "false", false
assert_eval "nil", "nil", nil

# node_str
assert_eval "empty string", '""', ""
assert_eval "string", '"hello"', "hello"

# node_self (top-level)
assert_eval "self at top", "self", nil
