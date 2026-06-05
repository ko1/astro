# Regression target for the `obj[i] = <Array/Hash literal>` / nested-index
# writeback bug (FIXED 2026-06-05).
#
# node_aref / node_aset / node_ary_new used to stage recv/idx/elements at the
# frame-top sp[0]/sp[1].  When an index expression was nested inside another
# (e.g. `arr[brr[0]]`, `h[k] = opt[:default]`, `h[1] = [9]`), the inner node
# staged at the SAME slots and clobbered the outer recv/idx, so the []= hit the
# wrong receiver (silently dropped, or NoMethodError).  Fix: stage at
# base = max(sp, c->sp_top) so a nested node (which bumps c->sp_top) lands
# above the enclosing node's slots.  See docs/spec_port_pending.md.
require_relative "test_helper"

def test_aset_array_literal
  h = {}
  h[1] = [9]
  assert_equal({ 1 => [9] }, h)
  a = [0]
  a[0] = [9]
  assert_equal([[9]], a)
end

def test_group_by
  g = [1, 2, 3].group_by { |x| x.odd? }
  assert_equal({ true => [1, 3], false => [2] }, g)
end

def test_nested_index_read
  arr = [10, 20, 30]
  idx = [2]
  assert_equal(30, arr[idx[0]])
end

def test_nested_index_write
  arr = [10, 20, 30]
  idx = [2]
  h = {}
  h[idx[0]] = arr[0]
  assert_equal({ 2 => 10 }, h)
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
  test_aset_array_literal test_group_by
  test_nested_index_read test_nested_index_write
  test_aset_variable_rhs_correct
]
TESTS.each { |t| run_test(t) }
report("AsetLiteral")
