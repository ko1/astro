require_relative "../../test_helper"

# Data.define tests inspired by CRuby's test_data.rb.
# Our impl reuses the Struct backbone — instances are not strictly
# frozen, so we only test the shape/access, not deep immutability.

Coord = Data.define(:x, :y)

def test_basic_data
  c = Coord.new(3, 4)
  assert_equal 3, c.x
  assert_equal 4, c.y
end

def test_to_a_data
  c = Coord.new(3, 4)
  assert_equal [3, 4], c.to_a
end

def test_eq_data
  assert_equal true,  Coord.new(3, 4) == Coord.new(3, 4)
  assert_equal false, Coord.new(3, 4) == Coord.new(3, 5)
end

def test_to_h_data
  c = Coord.new(3, 4)
  assert_equal({x: 3, y: 4}, c.to_h)
end

def test_data_with_block
  Vec = Data.define(:length, :angle) do
    def doubled; length * 2; end
  end
  v = Vec.new(5, 10)
  assert_equal 10, v.doubled
end

TESTS = [
  :test_basic_data, :test_to_a_data, :test_eq_data,
  :test_to_h_data, :test_data_with_block,
]
TESTS.each { |t| run_test(t) }
report "Data"
