require_relative 'test_helper'

class TestTier2Array < ArjsvTest
  def test_min_items
    s = schema('type' => 'array', 'minItems' => 2)
    assert_valid s, [1, 2]
    assert_valid s, [1, 2, 3]
    refute_valid s, []
    refute_valid s, [1]
  end

  def test_max_items
    s = schema('type' => 'array', 'maxItems' => 2)
    assert_valid s, []
    assert_valid s, [1, 2]
    refute_valid s, [1, 2, 3]
  end

  def test_unique_items
    s = schema('type' => 'array', 'uniqueItems' => true)
    assert_valid s, []
    assert_valid s, [1, 2, 3]
    assert_valid s, [{'a' => 1}, {'a' => 2}]
    refute_valid s, [1, 2, 1]
    refute_valid s, [{'a' => 1}, {'a' => 1}]
    # Bool != number per JSON Schema (1 and true compare unequal under uniqueItems).
    assert_valid s, [1, true]
    assert_valid s, [0, false]
  end

  def test_unique_items_false
    s = schema('type' => 'array', 'uniqueItems' => false)
    assert_valid s, [1, 1, 1]
  end

  def test_items_tuple
    s = schema(
      'type' => 'array',
      'items' => [{'type' => 'integer'}, {'type' => 'string'}],
    )
    assert_valid s, [1, 'a']
    assert_valid s, [1, 'a', 'whatever']      # extra ok with no additionalItems
    assert_valid s, [1]                       # short ok
    assert_valid s, []
    refute_valid s, ['x', 'a']                # idx 0 wrong type
    refute_valid s, [1, 2]                    # idx 1 wrong type
  end

  def test_items_tuple_no_additional
    s = schema(
      'type' => 'array',
      'items' => [{'type' => 'integer'}, {'type' => 'string'}],
      'additionalItems' => false,
    )
    assert_valid s, [1, 'a']
    assert_valid s, [1]
    assert_valid s, []
    refute_valid s, [1, 'a', 'x']             # too long
  end

  def test_items_tuple_with_additional_schema
    s = schema(
      'type' => 'array',
      'items' => [{'type' => 'integer'}],
      'additionalItems' => {'type' => 'string'},
    )
    assert_valid s, [1, 'a', 'b']
    assert_valid s, [1]
    refute_valid s, [1, 2]                    # idx 1 must be string
    refute_valid s, [1, 'a', 99]
  end
end
