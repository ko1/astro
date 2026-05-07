require_relative 'test_helper'

class TestTier2AdditionalProperties < ArjsvTest
  def test_additional_properties_false
    s = schema(
      'type' => 'object',
      'properties' => {'name' => {'type' => 'string'}},
      'additionalProperties' => false,
    )
    assert_valid s, {'name' => 'X'}
    assert_valid s, {}
    refute_valid s, {'name' => 'X', 'extra' => 1}
    refute_valid s, {'unknown' => 'whatever'}
  end

  def test_additional_properties_schema
    s = schema(
      'type' => 'object',
      'properties' => {'name' => {'type' => 'string'}},
      'additionalProperties' => {'type' => 'integer'},
    )
    assert_valid s, {'name' => 'X', 'age' => 30, 'count' => 42}
    assert_valid s, {'name' => 'X'}
    refute_valid s, {'name' => 'X', 'extra' => 'not int'}
  end

  def test_additional_properties_true_default
    s = schema(
      'type' => 'object',
      'properties' => {'name' => {'type' => 'string'}},
    )
    assert_valid s, {'name' => 'X', 'whatever' => [1, 2, 3]}
  end

  def test_pattern_properties
    s = schema(
      'type' => 'object',
      'patternProperties' => {
        '^S_' => {'type' => 'string'},
        '^I_' => {'type' => 'integer'},
      },
    )
    assert_valid s, {'S_name' => 'X', 'I_age' => 30}
    assert_valid s, {'random' => 'no constraint'}
    assert_valid s, {}
    refute_valid s, {'S_name' => 99}
    refute_valid s, {'I_age' => 'not int'}
  end

  def test_pattern_properties_with_additional_false
    s = schema(
      'type' => 'object',
      'patternProperties' => {'^x' => {'type' => 'integer'}},
      'additionalProperties' => false,
    )
    assert_valid s, {'x1' => 1, 'x2' => 2}
    assert_valid s, {}
    refute_valid s, {'x1' => 1, 'other' => 2}
    refute_valid s, {'x1' => 'not int'}
  end

  def test_property_names
    s = schema(
      'type' => 'object',
      'propertyNames' => {'pattern' => '^[A-Z]'},
    )
    assert_valid s, {'Foo' => 1, 'Bar' => 2}
    assert_valid s, {}
    refute_valid s, {'foo' => 1}
  end
end
