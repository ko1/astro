require_relative 'test_helper'

class TestValidSchema < ArjsvTest
  def test_module_valid_schema_accepts_well_formed_schema
    assert Arjsv.valid_schema?({'type' => 'integer'})
    assert Arjsv.valid_schema?({'type' => 'object', 'properties' => {'x' => {'type' => 'string'}}})
    assert Arjsv.valid_schema?({'allOf' => [{'type' => 'integer'}]})
    assert Arjsv.valid_schema?(true)
    assert Arjsv.valid_schema?(false)
  end

  def test_module_valid_schema_rejects_malformed_schema
    refute Arjsv.valid_schema?({'type' => 42})            # type must be String / Array<String>
    refute Arjsv.valid_schema?({'minLength' => -1})       # negative
    refute Arjsv.valid_schema?({'required' => 'name'})    # required must be Array
  end

  def test_instance_valid_schema
    s = Arjsv.schema({'type' => 'integer'})
    assert s.valid_schema?
  end
end
