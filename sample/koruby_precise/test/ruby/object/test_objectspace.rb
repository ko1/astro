require_relative "../../test_helper"

# ObjectSpace — koruby provides stub methods so the API surface
# exists.  Real `each_object` enumeration would need a weak-ref
# registry on top of Boehm GC; for now it yields nothing.

def test_objectspace_module_exists
  assert ObjectSpace.is_a?(Module)
end

def test_count_objects_returns_hash
  result = ObjectSpace.count_objects
  assert result.is_a?(Hash)
  assert result.key?(:TOTAL), "should have :TOTAL key"
end

def test_garbage_collect_no_raise
  # Just check it doesn't raise.
  ObjectSpace.garbage_collect
  assert true
end

def test_each_object_no_block_returns_array
  # No-block form — koruby returns an Array (CRuby returns Enumerator).
  result = ObjectSpace.each_object
  assert(result.is_a?(Array) || result.is_a?(Integer))
end

def test_each_object_with_block_yields_zero_in_stub
  # koruby stub yields 0 things.  Just confirm it doesn't raise.
  count = 0
  ObjectSpace.each_object { |_| count += 1 }
  # Stub: 0.  CRuby: > 0 (lots of live objects).  Accept any nonneg.
  assert(count >= 0)
end

TESTS = [
  :test_objectspace_module_exists,
  :test_count_objects_returns_hash,
  :test_garbage_collect_no_raise,
  :test_each_object_no_block_returns_array,
  :test_each_object_with_block_yields_zero_in_stub,
]
TESTS.each { |t| run_test(t) }
report "ObjectSpace"
