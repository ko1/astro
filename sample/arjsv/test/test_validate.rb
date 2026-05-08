require_relative 'test_helper'

class TestValidate < ArjsvTest
  def test_validate_returns_empty_for_valid_data
    s = Arjsv.schema('type' => 'integer', 'minimum' => 0)
    errors = s.validate(42).to_a
    assert_equal [], errors
  end

  def test_validate_returns_errors_for_invalid_data
    s = Arjsv.schema('type' => 'integer', 'minimum' => 0)
    errors = s.validate(-1).to_a
    refute_empty errors
    assert errors.first.is_a?(Hash), "errors should be Hashes (got #{errors.first.class})"
  end

  def test_validate_object_errors
    s = Arjsv.schema(
      'type' => 'object',
      'required' => ['name'],
      'properties' => {'name' => {'type' => 'string'}}
    )
    refute_empty s.validate({}).to_a               # missing required
    refute_empty s.validate({'name' => 1}).to_a    # wrong type
    assert_empty s.validate({'name' => 'X'}).to_a
  end

  def test_validate_returns_enumerator
    s = Arjsv.schema('type' => 'integer')
    result = s.validate('x')
    assert result.respond_to?(:to_a)
    assert result.respond_to?(:each)
  end
end
