require_relative 'test_helper'

class TestInsertDefaults < ArjsvTest
  # `insert_property_defaults` mutates the data hash to fill in `default`
  # values for absent properties.  json_schemer-compatible.
  def test_insert_defaults_basic
    s = Arjsv.schema(
      {
        'type' => 'object',
        'properties' => {
          'name' => {'type' => 'string'},
          'role' => {'type' => 'string', 'default' => 'user'},
          'tags' => {'type' => 'array',  'default' => []},
        },
      },
      insert_property_defaults: true,
    )
    data = {'name' => 'Alice'}
    assert s.valid?(data)
    assert_equal 'user', data['role']
    assert_equal [], data['tags']
    assert_equal 'Alice', data['name']        # untouched
  end

  def test_no_insert_when_property_present
    s = Arjsv.schema(
      {'type' => 'object',
       'properties' => {'role' => {'type' => 'string', 'default' => 'user'}}},
      insert_property_defaults: true,
    )
    data = {'role' => 'admin'}
    assert s.valid?(data)
    assert_equal 'admin', data['role']        # not overwritten
  end

  def test_insert_defaults_off_by_default
    s = Arjsv.schema(
      {'type' => 'object',
       'properties' => {'role' => {'type' => 'string', 'default' => 'user'}}}
    )
    data = {}
    assert s.valid?(data)
    refute data.key?('role')                  # data not mutated
  end

  def test_inserted_default_is_validated
    # Inserted default value still must satisfy the sub-schema (otherwise
    # the schema is buggy; we surface that as a validation failure).
    s = Arjsv.schema(
      {
        'type' => 'object',
        'properties' => {
          'count' => {'type' => 'integer', 'minimum' => 1, 'default' => 0},
        },
      },
      insert_property_defaults: true,
    )
    data = {}
    refute s.valid?(data)                     # default 0 < minimum 1 → fail
    assert_equal 0, data['count']             # but default still inserted
  end

  def test_default_with_symbol_key_data
    s = Arjsv.schema(
      {
        'type' => 'object',
        'properties' => {'role' => {'type' => 'string', 'default' => 'user'}},
      },
      insert_property_defaults: true,
    )
    data = {name: 'Alice'}                    # Symbol-keyed data
    assert s.valid?(data)
    # Default insertion uses the canonical String key form.  Mixed-key
    # hash is the natural result; document it.
    assert_equal 'user', data['role']
  end
end
