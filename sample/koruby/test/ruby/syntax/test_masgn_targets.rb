require_relative "../../test_helper"

# Regression: masgn used to silently drop targets that weren't plain
# variables.  Specifically `obj.x = ...` and `obj[k] = ...` on the LHS
# fell through and produced no assignment, which masked a render bug
# in optcarrot's PPU#setup_frame for ages.

class Box
  attr_accessor :x, :y, :z
end

def test_masgn_attr_setter_trailing
  box = Box.new
  a, b, box.x = [10, 20, 30]
  assert_equal 10, a
  assert_equal 20, b
  assert_equal 30, box.x
end

def test_masgn_attr_setter_leading
  box = Box.new
  box.x, a, b = [10, 20, 30]
  assert_equal 10, box.x
  assert_equal 20, a
  assert_equal 30, b
end

def test_masgn_attr_setter_multiple
  b1 = Box.new
  b2 = Box.new
  b1.x, b2.y = [11, 22]
  assert_equal 11, b1.x
  assert_equal 22, b2.y
end

def test_masgn_attr_setter_with_splat
  box = Box.new
  a, *box.x, b = [1, 2, 3, 4, 5]
  assert_equal 1, a
  assert_equal [2, 3, 4], box.x
  assert_equal 5, b
end

def test_masgn_index_setter_trailing
  arr = [0, 0, 0]
  a, arr[1], c = [10, 20, 30]
  assert_equal 10, a
  assert_equal [0, 20, 0], arr
  assert_equal 30, c
end

def test_masgn_index_setter_hash
  h = {}
  a, h[:k], c = [1, 2, 3]
  assert_equal 1, a
  assert_equal 2, h[:k]
  assert_equal 3, c
end

def test_masgn_mixed_targets
  box = Box.new
  arr = [0, 0]
  v, box.x, arr[0] = [100, 200, 300]
  assert_equal 100, v
  assert_equal 200, box.x
  assert_equal 300, arr[0]
end

# The optcarrot trigger shape: three setters, last one being an
# attribute on a freshly-constructed object.
class Clock
  attr_accessor :next_frame
end

def test_masgn_optcarrot_shape
  c = Clock.new
  c.next_frame = 0
  vclk = nil; hclk = nil
  vclk, hclk, c.next_frame = [42, 1234, 328608]
  assert_equal 42, vclk
  assert_equal 1234, hclk
  assert_equal 328608, c.next_frame
end

TESTS = [
  :test_masgn_attr_setter_trailing,
  :test_masgn_attr_setter_leading,
  :test_masgn_attr_setter_multiple,
  :test_masgn_attr_setter_with_splat,
  :test_masgn_index_setter_trailing,
  :test_masgn_index_setter_hash,
  :test_masgn_mixed_targets,
  :test_masgn_optcarrot_shape,
]
TESTS.each { |t| run_test(t) }
report "MasgnTargets"
