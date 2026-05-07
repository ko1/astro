require_relative 'test_helper'

class TestEnumConst < ArjsvTest
  def test_enum_strings
    s = schema('enum' => ['red', 'green', 'blue'])
    assert_valid s, 'red'
    assert_valid s, 'green'
    refute_valid s, 'yellow'
    refute_valid s, 0
  end

  def test_enum_mixed_types
    s = schema('enum' => [1, 'one', nil, true])
    assert_valid s, 1
    assert_valid s, 'one'
    assert_valid s, nil
    assert_valid s, true
    refute_valid s, 2
    refute_valid s, 'two'
    refute_valid s, false
  end

  def test_const_string
    s = schema('const' => 'OK')
    assert_valid s, 'OK'
    refute_valid s, 'ok'
    refute_valid s, 'OKK'
    refute_valid s, 0
  end

  def test_const_object
    s = schema('const' => {'a' => 1})
    assert_valid s, {'a' => 1}
    refute_valid s, {'a' => 2}
    refute_valid s, {}
    refute_valid s, {'a' => 1, 'b' => 2}
  end

  def test_const_array
    s = schema('const' => [1, 2, 3])
    assert_valid s, [1, 2, 3]
    refute_valid s, [1, 2]
    refute_valid s, [1, 2, 3, 4]
  end
end
