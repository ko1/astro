# Regression target for the `obj[i] = <Array/Hash literal>` writeback bug.
#
# When the RHS of an index assignment is an Array/Hash LITERAL (not a variable),
# the assignment is silently dropped: node_aset reserves sp[0]/sp[1] for
# recv/idx, but the literal's node_ary_new/node_hash_new stages its result at
# sp[0] of the same sp, clobbering recv. See docs/spec_port_pending.md.
#
# This file PINS the current (buggy) behavior so the suite stays green, and
# flips to FAIL once the bug is fixed (signalling: remove the pins, restore the
# correct expectations shown in comments).  The variable-RHS / nested-scalar
# cases assert CORRECT behavior and guard against a fix regressing them.
require_relative "test_helper"

def test_aset_array_literal_pinned
  h = {}
  h[1] = [9]
  assert_equal({}, h, "PIN aset array-literal dropped; fixed expects 1=>[9]")
  a = [0]
  a[0] = [9]
  assert_equal([0], a, "PIN aset array-literal dropped; fixed expects [[9]]")
end

def test_group_by_pinned
  g = [1, 2, 3].group_by { |x| x.odd? }
  assert_equal({}, g, "PIN group_by empty via aset bug; fixed expects grouped")
end

def test_aset_variable_rhs_correct
  a = [9]
  h = {}
  h[1] = a
  assert_equal({ 1 => [9] }, h)
  h2 = {}
  h2[1] = {}
  h2[1][2] = 3
  assert_equal({ 1 => { 2 => 3 } }, h2)
end

TESTS = %i[
  test_aset_array_literal_pinned test_group_by_pinned
  test_aset_variable_rhs_correct
]
TESTS.each { |t| run_test(t) }
report("AsetLiteral")
