require_relative "../../test_helper"

# Frozen semantics: modifying a frozen object should raise FrozenError.

def test_modify_frozen_string_raises
  s = "x".freeze
  raised = false
  begin
    s << "y"
  rescue FrozenError, RuntimeError
    raised = true
  end
  assert raised, "expected FrozenError modifying frozen string"
end

def test_modify_frozen_array_raises
  a = [1, 2].freeze
  raised = false
  begin
    a << 3
  rescue FrozenError, RuntimeError
    raised = true
  end
  assert raised, "expected FrozenError modifying frozen array"
end

def test_modify_frozen_hash_raises
  h = {a: 1}.freeze
  raised = false
  begin
    h[:b] = 2
  rescue FrozenError, RuntimeError
    raised = true
  end
  assert raised, "expected FrozenError modifying frozen hash"
end

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
  :test_modify_frozen_string_raises,
  :test_modify_frozen_array_raises,
  :test_modify_frozen_hash_raises,
  :test_dup_does_not_inherit_freeze,
  :test_numeric_frozen,
  :test_symbol_frozen,
  :test_nil_true_false_frozen,
]
TESTS.each { |t| run_test(t) }
report "FrozenDeep"
