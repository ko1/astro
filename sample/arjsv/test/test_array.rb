require_relative 'test_helper'

class TestArray < ArjsvTest
  def test_items_uniform
    s = schema('type' => 'array', 'items' => {'type' => 'integer'})
    assert_valid s, []
    assert_valid s, [1, 2, 3]
    refute_valid s, [1, '2']
    refute_valid s, ['x']
  end

  def test_items_with_constraints
    s = schema(
      'type' => 'array',
      'items' => {'type' => 'integer', 'minimum' => 0, 'maximum' => 100},
    )
    assert_valid s, [0, 50, 100]
    refute_valid s, [-1]
    refute_valid s, [101]
  end

  def test_items_nested
    s = schema(
      'type' => 'array',
      'items' => {
        'type' => 'object',
        'required' => ['id'],
        'properties' => {'id' => {'type' => 'integer'}},
      }
    )
    assert_valid s, [{'id' => 1}, {'id' => 2}]
    refute_valid s, [{'id' => 1}, {}]
    refute_valid s, [{'id' => 'x'}]
  end

  def test_items_on_non_array_skipped
    s = schema('items' => {'type' => 'integer'})
    assert_valid s, 'string'
    assert_valid s, 42
    assert_valid s, []
  end
end
