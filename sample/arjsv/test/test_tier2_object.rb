require_relative 'test_helper'

class TestTier2Object < ArjsvTest
  def test_min_properties
    s = schema('type' => 'object', 'minProperties' => 2)
    assert_valid s, {'a' => 1, 'b' => 2}
    assert_valid s, {'a' => 1, 'b' => 2, 'c' => 3}
    refute_valid s, {}
    refute_valid s, {'a' => 1}
  end

  def test_max_properties
    s = schema('type' => 'object', 'maxProperties' => 2)
    assert_valid s, {}
    assert_valid s, {'a' => 1, 'b' => 2}
    refute_valid s, {'a' => 1, 'b' => 2, 'c' => 3}
  end
end
