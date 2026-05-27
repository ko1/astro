require_relative "../../test_helper"

# Comparable mixin in user-defined classes — relies on <=> dispatching
# to the comparison operators / between? / clamp / etc.

class Version
  include Comparable
  attr_reader :major, :minor, :patch
  def initialize(s)
    a = s.split(".").map(&:to_i)
    @major, @minor, @patch = a[0], a[1], a[2]
  end
  def <=>(other)
    return nil unless other.is_a?(Version)
    [major, minor, patch] <=> [other.major, other.minor, other.patch]
  end
end

V100 = Version.new("1.0.0")
V110 = Version.new("1.1.0")
V200 = Version.new("2.0.0")

def test_lt
  assert_equal true,  V100 < V110
  assert_equal false, V200 < V110
end

def test_le
  assert_equal true, V100 <= V100
  assert_equal true, V100 <= V110
end

def test_gt
  assert_equal true, V200 > V110
end

def test_eq_via_spaceship
  # Comparable's == returns true when <=> returns 0.
  v = Version.new("1.0.0")
  assert_equal true, V100 == v
end

def test_between
  assert_equal true,  V110.between?(V100, V200)
  assert_equal false, V100.between?(V110, V200)
end

def test_min_max_via_array
  versions = [V200, V100, V110]
  assert_equal V100, versions.min
  assert_equal V200, versions.max
end

def test_sort
  assert_equal [V100, V110, V200], [V200, V100, V110].sort
end

TESTS = [
  :test_lt, :test_le, :test_gt,
  :test_eq_via_spaceship,
  :test_between,
  :test_min_max_via_array,
  :test_sort,
]
TESTS.each { |t| run_test(t) }
report "CustomComparable"
