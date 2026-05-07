require_relative 'test_helper'

class TestTier2Numeric < ArjsvTest
  def test_multiple_of_integer
    s = schema('multipleOf' => 3)
    assert_valid s, 0
    assert_valid s, 3
    assert_valid s, -9
    refute_valid s, 1
    refute_valid s, 7
  end

  def test_multiple_of_fractional
    s = schema('multipleOf' => 0.5)
    assert_valid s, 0
    assert_valid s, 1.5
    assert_valid s, 2
    refute_valid s, 1.25
  end

  def test_multiple_of_skips_non_numeric
    s = schema('multipleOf' => 3)
    assert_valid s, 'x'
    assert_valid s, []
    assert_valid s, nil
  end
end
