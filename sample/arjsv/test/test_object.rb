require_relative 'test_helper'

class TestObject < ArjsvTest
  def test_required
    s = schema('type' => 'object', 'required' => ['name', 'age'])
    assert_valid s, {'name' => 'A', 'age' => 1}
    assert_valid s, {'name' => 'A', 'age' => 1, 'extra' => 9}
    refute_valid s, {'name' => 'A'}
    refute_valid s, {'age' => 1}
    refute_valid s, {}
  end

  # Symbol-key data (e.g. JSON.parse(symbolize_names: true), Rails params)
  # validates the same as String-key data.  String-key is the spec form and
  # the runtime fast path; Symbol-key falls back to a second hash lookup
  # only when the String lookup misses.
  def test_required_with_symbol_keys
    s = schema('type' => 'object', 'required' => ['name', 'age'])
    assert_valid s, {name: 'A', age: 1}
    refute_valid s, {name: 'A'}
    refute_valid s, {}
  end

  def test_properties_with_symbol_keys
    s = schema(
      'type' => 'object',
      'properties' => {
        'name' => {'type' => 'string'},
        'age'  => {'type' => 'integer'},
      }
    )
    assert_valid s, {name: 'X', age: 30}
    refute_valid s, {name: 1}
    refute_valid s, {name: 'X', age: 'thirty'}
  end

  # Schemas can themselves use Symbol keys.  Schema-build-time normalises
  # Symbol keys at schema positions to Strings (one-shot); enum / const
  # values keep their original key convention so user-side data matching
  # works as intended.
  def test_symbol_keyed_schema
    s = schema(
      type: 'object',
      required: ['name'],
      properties: {
        'name' => {type: 'string', minLength: 1},
      },
    )
    assert_valid s, {'name' => 'X'}
    refute_valid s, {'name' => ''}                # minLength fails
    refute_valid s, {}                            # required fails
  end

  def test_symbol_keyed_schema_with_symbol_data
    # Both schema and data Symbol-keyed — the common Rails / Sinatra flow.
    s = schema(
      type: 'object',
      required: ['age'],
      properties: {
        'age' => {type: 'integer', minimum: 0},
      },
    )
    assert_valid s, {age: 18}
    refute_valid s, {age: -1}
    refute_valid s, {}
  end

  def test_required_on_non_object_skipped
    # `required` only triggers when value is an object — for non-objects the
    # type check (a sibling) is what fails.  required alone passes them.
    s = schema('required' => ['name'])
    assert_valid s, 'no object'
    assert_valid s, 42
    refute_valid s, {}
    refute_valid s, {'other' => 1}
  end

  def test_properties
    s = schema(
      'type' => 'object',
      'properties' => {
        'name' => {'type' => 'string'},
        'age'  => {'type' => 'integer', 'minimum' => 0},
      }
    )
    assert_valid s, {'name' => 'A', 'age' => 30}
    assert_valid s, {'name' => 'A'}                          # absent age = ok
    assert_valid s, {}                                       # both absent
    refute_valid s, {'name' => 1}                            # name wrong type
    refute_valid s, {'name' => 'A', 'age' => -1}             # age below minimum
    refute_valid s, {'name' => 'A', 'age' => 'thirty'}       # age wrong type
  end

  def test_properties_and_required_combined
    s = schema(
      'type' => 'object',
      'required' => ['name'],
      'properties' => {
        'name' => {'type' => 'string', 'minLength' => 1},
      }
    )
    assert_valid s, {'name' => 'X'}
    refute_valid s, {'name' => ''}                           # too short
    refute_valid s, {}                                       # missing
    refute_valid s, {'name' => 0}                            # wrong type
  end

  def test_nested_object
    s = schema(
      'type' => 'object',
      'properties' => {
        'addr' => {
          'type' => 'object',
          'required' => ['city'],
          'properties' => {'city' => {'type' => 'string'}},
        },
      }
    )
    assert_valid s, {'addr' => {'city' => 'Tokyo'}}
    assert_valid s, {}                              # addr absent
    refute_valid s, {'addr' => {}}                  # missing required city
    refute_valid s, {'addr' => {'city' => 1}}       # wrong city type
    refute_valid s, {'addr' => 'Tokyo'}             # addr wrong type
  end
end
