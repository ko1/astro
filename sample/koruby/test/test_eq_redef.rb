# Tests for the basic-op redefinition guard on ==/!=.
# Runs in its own process because once Integer#== is redefined, every
# subsequent `==` (including assert_equal) takes the new path.  We
# therefore can't share a process with other test files.
#
# Strategy: capture the expected results BEFORE the redef takes effect,
# verify them with `==`-using helpers BEFORE the def, and then exercise
# the post-def behavior with raw equality writes.

# Phase 1 — sanity baseline, asserts before any redef.
require_relative "test_helper"

def test_pre_redef_baseline
  assert_equal true, 1 == 1
  assert_equal false, 1 == 2
end

$current = :test_pre_redef_baseline; test_pre_redef_baseline

# Phase 2 — redefine Integer#== to return 99 for any operand.  Must
# stop using == in assertions afterwards (would loop into the redef).
class Integer
  def ==(other); 99; end
end

# Phase 3 — verify post-redef behavior using `equal?` (object identity)
# which is unaffected.
def assert_equal_id(expected, actual)
  if expected.equal?(actual)
    $pass += 1
  else
    $fail += 1
    puts "FAIL #{$current}: expected #{expected.inspect}, got #{actual.inspect}"
  end
end

def test_post_redef_eq_returns_99
  $current = :test_post_redef_eq_returns_99
  assert_equal_id 99, (1 == 1)
  assert_equal_id 99, (1 == 2)
  assert_equal_id 99, (0 == nil)
end

def test_post_redef_neq_inverts_redef_result
  $current = :test_post_redef_neq_inverts_redef_result
  # `1 != 2` evaluates as !RTEST(1.==(2)) = !RTEST(99) = false.
  assert_equal_id false, (1 != 2)
  assert_equal_id false, (1 != 1)
end

test_post_redef_eq_returns_99
test_post_redef_neq_inverts_redef_result

if $fail == 0
  puts "OK EqRedef (#{$pass})"
else
  puts "FAIL EqRedef: #{$fail}/#{$pass + $fail}"
end
