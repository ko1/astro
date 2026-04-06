require 'minitest/autorun'
require_relative '../lib/abruby'

class AbRubyTest < Minitest::Test
  def assert_eval(code, expected)
    result = AbRuby.eval(code)
    if expected.nil?
      assert_nil result, "AbRuby.eval(#{code.inspect})"
    else
      assert_equal expected, result, "AbRuby.eval(#{code.inspect})"
    end
  end
end
