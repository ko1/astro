require_relative "../../test_helper"

# dup / clone / freeze / frozen? semantics.

# ---------- dup ----------

def test_dup_creates_new_object
  a = [1, 2, 3]
  b = a.dup
  assert_equal a, b
  assert(a.object_id != b.object_id, "dup should be a different object")
end

def test_dup_does_not_share_array_storage
  a = [1, 2, 3]
  b = a.dup
  b << 4
  assert_equal [1, 2, 3], a
  assert_equal [1, 2, 3, 4], b
end

def test_dup_hash
  h = {a: 1}
  k = h.dup
  k[:b] = 2
  assert_equal({a: 1}, h)
  assert_equal({a: 1, b: 2}, k)
end

def test_dup_string
  s = "hello"
  t = s.dup
  t << " world"
  assert_equal "hello",        s
  assert_equal "hello world",  t
end

# ---------- clone ----------

def test_clone_creates_new_object
  a = [1, 2]
  b = a.clone
  assert(a.object_id != b.object_id)
  assert_equal a, b
end

# ---------- frozen? / freeze ----------

def test_frozen_default_false
  assert_equal false, "x".frozen?
  assert_equal false, [1].frozen?
end

def test_freeze_sets_frozen
  s = "x"
  s.freeze
  assert_equal true, s.frozen?
end

def test_symbol_is_frozen
  # All symbols are inherently frozen.
  assert_equal true, :foo.frozen?
end

def test_frozen_integer
  # Numerics are frozen (immutable).
  assert_equal true, 1.frozen?
  assert_equal true, 1.0.frozen?
end

TESTS = [
  :test_dup_creates_new_object,
  :test_dup_does_not_share_array_storage,
  :test_dup_hash,
  :test_dup_string,
  :test_clone_creates_new_object,
  :test_frozen_default_false,
  :test_freeze_sets_frozen,
  :test_symbol_is_frozen,
  :test_frozen_integer,
]
TESTS.each { |t| run_test(t) }
report "DupCloneFrozen"
