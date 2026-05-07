require_relative 'test_helper'

class TestTier2Ref < ArjsvTest
  def test_simple_ref
    s = schema(
      '$defs' => {
        'PositiveInt' => {'type' => 'integer', 'minimum' => 0},
      },
      '$ref' => '#/$defs/PositiveInt',
    )
    assert_valid s, 1
    assert_valid s, 0
    refute_valid s, -1
    refute_valid s, 'x'
  end

  def test_definitions_alias
    s = schema(
      'definitions' => {
        'NonEmpty' => {'type' => 'string', 'minLength' => 1},
      },
      '$ref' => '#/definitions/NonEmpty',
    )
    assert_valid s, 'x'
    refute_valid s, ''
    refute_valid s, 0
  end

  def test_ref_used_in_subschema
    s = schema(
      '$defs' => {
        'Address' => {
          'type' => 'object',
          'required' => ['city'],
          'properties' => {'city' => {'type' => 'string'}},
        },
      },
      'type' => 'object',
      'required' => ['home'],
      'properties' => {
        'home' => {'$ref' => '#/$defs/Address'},
        'work' => {'$ref' => '#/$defs/Address'},
      },
    )
    assert_valid s, {'home' => {'city' => 'Tokyo'}}
    assert_valid s, {'home' => {'city' => 'Tokyo'}, 'work' => {'city' => 'Osaka'}}
    refute_valid s, {'home' => {}}
    refute_valid s, {'home' => {'city' => 99}}
    refute_valid s, {'home' => {'city' => 'Tokyo'}, 'work' => {}}
  end

  def test_recursive_ref
    s = schema(
      '$defs' => {
        'Tree' => {
          'type' => 'object',
          'required' => ['value'],
          'properties' => {
            'value' => {'type' => 'integer'},
            'children' => {
              'type' => 'array',
              'items' => {'$ref' => '#/$defs/Tree'},
            },
          },
        },
      },
      '$ref' => '#/$defs/Tree',
    )
    assert_valid s, {'value' => 1}
    assert_valid s, {'value' => 1, 'children' => []}
    assert_valid s, {'value' => 1, 'children' => [{'value' => 2}, {'value' => 3, 'children' => [{'value' => 4}]}]}
    refute_valid s, {}                                                      # missing value
    refute_valid s, {'value' => 'x'}                                        # value wrong type
    refute_valid s, {'value' => 1, 'children' => [{'value' => 'x'}]}        # nested wrong type
    refute_valid s, {'value' => 1, 'children' => [{}]}                      # nested missing value
  end

  def test_ref_siblings_ignored_draft07
    # In draft-07, siblings of $ref are ignored.
    s = schema(
      '$defs' => {'Pos' => {'type' => 'integer', 'minimum' => 0}},
      '$ref' => '#/$defs/Pos',
      'maximum' => 10,    # SHOULD be ignored
    )
    assert_valid s, 100   # not constrained by maximum
    refute_valid s, -1
  end
end
