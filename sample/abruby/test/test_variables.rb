require_relative 'test_helper'

# node_lget / node_lset
assert_eval "simple assign", "a = 1; a", 1
assert_eval "two vars", "a = 1; b = 2; a + b", 3
assert_eval "reassign", "a = 1; a = 2; a", 2
assert_eval "assign returns value", "a = 42", 42

# compound assignment (uses method_call internally)
assert_eval "+= on int", "a = 1; a += 2; a", 3
assert_eval "-= on int", "a = 10; a -= 3; a", 7
assert_eval "*= on int", "a = 5; a *= 3; a", 15

# node_scope
assert_eval "scope preserves outer", "a = 1; b = 2; a", 1

# multiple assignments
assert_eval "chain", "a = 1; b = a; c = b; c", 1
