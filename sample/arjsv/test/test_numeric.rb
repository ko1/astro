require_relative 'test_helper'

class TestNumeric < ArjsvTest
  def test_minimum
    s = schema('minimum' => 0)
    assert_valid s, 0
    assert_valid s, 1
    assert_valid s, 100.5
    refute_valid s, -1
    refute_valid s, -0.001
  end

  def test_maximum
    s = schema('maximum' => 10)
    assert_valid s, 10
    assert_valid s, -100
    refute_valid s, 11
    refute_valid s, 10.0001
  end

  def test_exclusive_minimum_numeric_draft07
    s = schema('exclusiveMinimum' => 0)
    refute_valid s, 0
    assert_valid s, 0.0001
    assert_valid s, 1
  end

  def test_exclusive_maximum_numeric_draft07
    s = schema('exclusiveMaximum' => 10)
    refute_valid s, 10
    assert_valid s, 9.9999
  end

  def test_minimum_skipped_for_non_numeric
    s = schema('minimum' => 5)
    assert_valid s, 'x'
    assert_valid s, nil
    assert_valid s, []
  end

  def test_minimum_and_maximum_combined
    s = schema('type' => 'integer', 'minimum' => 1, 'maximum' => 10)
    assert_valid s, 1
    assert_valid s, 10
    assert_valid s, 5
    refute_valid s, 0
    refute_valid s, 11
    refute_valid s, 'x'  # type fails
  end
end
