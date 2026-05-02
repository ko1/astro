# Tests for Comparable mixin — derive < <= > >= == between? clamp from <=>.

require_relative "../../test_helper"

class Box
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(other); v <=> other.v; end
end

A = Box.new(1)
B = Box.new(2)
C = Box.new(3)

def test_lt
  assert_equal true, A < B
  assert_equal false, B < A
  assert_equal false, A < A
end

def test_le
  assert_equal true, A <= B
  assert_equal true, A <= A
  assert_equal false, B <= A
end

def test_gt
  assert_equal true, B > A
  assert_equal false, A > B
end

def test_ge
  assert_equal true, B >= A
  assert_equal true, A >= A
  assert_equal false, A >= B
end

def test_eq
  assert_equal true, A == Box.new(1)
  assert_equal false, A == B
end

def test_between
  assert_equal true, B.between?(A, C)
  assert_equal false, A.between?(B, C)
  assert_equal true, A.between?(A, C)
end

def test_clamp
  # clamp(min, max) — return self if in range, else the boundary
  assert_equal B.v, B.clamp(A, C).v
  assert_equal A.v, Box.new(0).clamp(A, C).v
  assert_equal C.v, Box.new(99).clamp(A, C).v
end

def test_min_max_via_array_sort
  ary = [B, A, C]
  assert_equal A, ary.min
  assert_equal C, ary.max
end

TESTS = [
  :test_lt, :test_le, :test_gt, :test_ge, :test_eq,
  :test_between, :test_clamp,
  :test_min_max_via_array_sort,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK Comparable (#{$pass})"
else
  puts "FAIL Comparable: #{$fail}/#{$pass + $fail}"
end
