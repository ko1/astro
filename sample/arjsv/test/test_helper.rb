require 'minitest/autorun'
require_relative '../lib/arjsv'

ARJSV_MODE = ENV['ARJSV_MODE'] || 'plain'

class ArjsvTest < Minitest::Test
  def schema(s)
    sch = Arjsv.schema(s)
    sch.compile! if ARJSV_MODE == 'compiled'
    sch
  end

  def assert_valid(schema_obj, data, msg = nil)
    sch = schema_obj.is_a?(Arjsv::Schema) ? schema_obj : schema(schema_obj)
    assert sch.valid?(data),
           msg || "expected #{data.inspect} to be valid against #{schema_obj.inspect}"
  end

  def refute_valid(schema_obj, data, msg = nil)
    sch = schema_obj.is_a?(Arjsv::Schema) ? schema_obj : schema(schema_obj)
    refute sch.valid?(data),
           msg || "expected #{data.inspect} to be INVALID against #{schema_obj.inspect}"
  end
end
