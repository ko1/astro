require_relative "../../test_helper"

# Struct tests inspired by CRuby's test_struct.rb.

Point = Struct.new(:x, :y)

def test_basic
  p = Point.new(3, 4)
  assert_equal 3, p.x
  assert_equal 4, p.y
end

def test_to_a
  p = Point.new(3, 4)
  assert_equal [3, 4], p.to_a
end

def test_members
  assert_equal [:x, :y], Point.members
end

def test_set_attr
  p = Point.new(3, 4)
  p.x = 99
  assert_equal 99, p.x
end

def test_aref
  p = Point.new(3, 4)
  assert_equal 3, p[0]
  assert_equal 4, p[1]
  assert_equal 3, p[:x]
end

def test_aset
  p = Point.new(3, 4)
  p[0] = 99
  assert_equal 99, p.x
end

def test_each
  p = Point.new(3, 4)
  collected = []
  p.each { |v| collected << v }
  assert_equal [3, 4], collected
end

def test_eq
  assert_equal true,  Point.new(3, 4) == Point.new(3, 4)
  assert_equal false, Point.new(3, 4) == Point.new(3, 5)
end

def test_to_h
  p = Point.new(3, 4)
  assert_equal({x: 3, y: 4}, p.to_h)
end

def test_size
  p = Point.new(3, 4)
  assert_equal 2, p.size
  assert_equal 2, p.length
end

def test_struct_with_block
  K = Struct.new(:name) do
    def shout; name.upcase; end
  end
  assert_equal "BOB", K.new("bob").shout
end

TESTS = [
  :test_basic, :test_to_a, :test_members, :test_set_attr,
  :test_aref, :test_aset, :test_each, :test_eq,
  :test_to_h, :test_size, :test_struct_with_block,
]
TESTS.each { |t| run_test(t) }
report "Struct"
