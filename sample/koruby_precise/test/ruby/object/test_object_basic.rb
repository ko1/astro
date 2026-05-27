require_relative "../../test_helper"

class P
  attr_accessor :x, :y
  def initialize(x, y); @x = x; @y = y; end
end

def test_dup_independent
  p = P.new(1, 2)
  q = p.dup
  q.x = 99
  assert_equal 1, p.x
  assert_equal 99, q.x
end

def test_clone
  p = P.new(1, 2)
  q = p.clone
  q.y = 88
  assert_equal 2, p.y
  assert_equal 88, q.y
end

def test_eql_default_is_equal
  a = "hi"
  b = "hi"
  assert_equal true, a.eql?(a)
  # eql? on different String objects with same content — true (== too).
  assert_equal true, a.eql?(b)
end

def test_equal_object_identity
  s = "hi"
  assert_equal true, s.equal?(s)
  # Distinct objects: dup makes a fresh one even if literal pooling
  # happens to share underlying storage.
  assert_equal false, s.equal?(s.dup)
end

def test_hash_returns_integer
  assert_equal true, "abc".hash.is_a?(Integer)
  assert_equal true, [1, 2].hash.is_a?(Integer)
end

# Same content ⇒ same hash for built-ins
def test_hash_same_content
  assert_equal "abc".hash, "abc".hash
  assert_equal [1, 2].hash, [1, 2].hash
end

TESTS = [
  :test_dup_independent,
  :test_clone,
  :test_eql_default_is_equal,
  :test_equal_object_identity,
  :test_hash_returns_integer,
  :test_hash_same_content,
]

TESTS.each { |t| run_test(t) }
report "ObjectBasic"
