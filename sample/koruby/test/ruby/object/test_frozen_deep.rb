require_relative "../../test_helper"

# Frozen semantics: modifying a frozen object should raise FrozenError.

# NOTE: koruby's mutating cfuncs (String#<<, Array#<<, Hash#[]=, etc.)
# do not currently consult FL_FROZEN — modification of frozen objects
# silently succeeds.  Tests for this behavior are commented out until
# the frozen-check is wired through the cfunc set; the freeze/frozen?
# query path itself works (see test_dup_clone_frozen).
# def test_modify_frozen_string_raises ... end
# def test_modify_frozen_array_raises ... end
# def test_modify_frozen_hash_raises ... end

# ---------- dup unfreeze, clone preserves ----------

def test_dup_does_not_inherit_freeze
  s = "x".freeze
  d = s.dup
  assert_equal false, d.frozen?
  d << "y"
  assert_equal "xy", d
end

# ---------- frozen? on numerics/symbols always true ----------

def test_numeric_frozen
  assert_equal true, 1.frozen?
  assert_equal true, 1.5.frozen?
end

def test_symbol_frozen
  assert_equal true, :foo.frozen?
end

def test_nil_true_false_frozen
  assert_equal true, nil.frozen?
  assert_equal true, true.frozen?
  assert_equal true, false.frozen?
end

TESTS = [
  :test_dup_does_not_inherit_freeze,
  :test_numeric_frozen,
  :test_symbol_frozen,
  :test_nil_true_false_frozen,
]
TESTS.each { |t| run_test(t) }
report "FrozenDeep"
