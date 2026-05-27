require_relative "../../test_helper"

# Frozen check on the broader mutator surface.

def assert_raises_frozen
  raised = false
  begin
    yield
  rescue FrozenError, RuntimeError
    raised = true
  end
  assert raised, "expected FrozenError"
end

# ---------- Array mutators ----------

def test_frozen_array_unshift
  a = [1, 2].freeze
  assert_raises_frozen { a.unshift(0) }
end

def test_frozen_array_shift
  a = [1, 2].freeze
  assert_raises_frozen { a.shift }
end

def test_frozen_array_clear
  a = [1, 2].freeze
  assert_raises_frozen { a.clear }
end

def test_frozen_array_concat
  a = [1].freeze
  assert_raises_frozen { a.concat([2, 3]) }
end

def test_frozen_array_delete
  a = [1, 2].freeze
  assert_raises_frozen { a.delete(1) }
end

def test_frozen_array_insert
  a = [1, 2].freeze
  assert_raises_frozen { a.insert(0, :x) }
end

def test_frozen_array_replace
  a = [1].freeze
  assert_raises_frozen { a.replace([2]) }
end

# ---------- Hash mutators ----------

def test_frozen_hash_delete
  h = {a: 1}.freeze
  assert_raises_frozen { h.delete(:a) }
end

def test_frozen_hash_clear
  h = {a: 1}.freeze
  assert_raises_frozen { h.clear }
end

def test_frozen_hash_merge_bang
  h = {a: 1}.freeze
  assert_raises_frozen { h.merge!(b: 2) }
end

def test_frozen_hash_replace
  h = {a: 1}.freeze
  assert_raises_frozen { h.replace(b: 2) }
end

# ---------- String mutators ----------

def test_frozen_string_prepend
  s = "world".freeze
  assert_raises_frozen { s.prepend("hello, ") }
end

def test_frozen_string_insert
  s = "abc".freeze
  assert_raises_frozen { s.insert(1, "X") }
end

def test_frozen_string_replace
  s = "x".freeze
  assert_raises_frozen { s.replace("y") }
end

# ---------- non-frozen still works ----------

def test_normal_unshift_still_works
  a = [1, 2]
  a.unshift(0)
  assert_equal [0, 1, 2], a
end

def test_normal_hash_delete_still_works
  h = {a: 1, b: 2}
  h.delete(:a)
  assert_equal({b: 2}, h)
end

def test_normal_string_prepend_still_works
  s = "world"
  s.prepend("hello, ")
  assert_equal "hello, world", s
end

TESTS = [
  :test_frozen_array_unshift, :test_frozen_array_shift, :test_frozen_array_clear,
  :test_frozen_array_concat, :test_frozen_array_delete, :test_frozen_array_insert,
  :test_frozen_array_replace,
  :test_frozen_hash_delete, :test_frozen_hash_clear, :test_frozen_hash_merge_bang,
  :test_frozen_hash_replace,
  :test_frozen_string_prepend, :test_frozen_string_insert, :test_frozen_string_replace,
  :test_normal_unshift_still_works, :test_normal_hash_delete_still_works,
  :test_normal_string_prepend_still_works,
]
TESTS.each { |t| run_test(t) }
report "FrozenMutators"
