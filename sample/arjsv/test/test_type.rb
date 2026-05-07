require_relative 'test_helper'

class TestType < ArjsvTest
  def test_string
    s = schema('type' => 'string')
    assert_valid s, 'hello'
    assert_valid s, ''
    refute_valid s, 42
    refute_valid s, nil
    refute_valid s, true
    refute_valid s, []
    refute_valid s, {}
  end

  def test_integer
    s = schema('type' => 'integer')
    assert_valid s, 0
    assert_valid s, -100
    assert_valid s, 1 << 70           # bignum
    assert_valid s, 3.0               # integer-valued float
    refute_valid s, 1.5
    refute_valid s, '1'
    refute_valid s, true              # bool is not a number
    refute_valid s, nil
  end

  def test_number
    s = schema('type' => 'number')
    assert_valid s, 1
    assert_valid s, 1.5
    assert_valid s, -3.14
    refute_valid s, '1'
    refute_valid s, true
    refute_valid s, nil
  end

  def test_boolean
    s = schema('type' => 'boolean')
    assert_valid s, true
    assert_valid s, false
    refute_valid s, 1
    refute_valid s, 0
    refute_valid s, 'true'
  end

  def test_null
    s = schema('type' => 'null')
    assert_valid s, nil
    refute_valid s, 0
    refute_valid s, false
    refute_valid s, ''
  end

  def test_array
    s = schema('type' => 'array')
    assert_valid s, []
    assert_valid s, [1, 2, 3]
    refute_valid s, {}
    refute_valid s, '[]'
  end

  def test_object
    s = schema('type' => 'object')
    assert_valid s, {}
    assert_valid s, {'a' => 1}
    refute_valid s, []
    refute_valid s, '{}'
  end

  def test_union_type
    s = schema('type' => ['string', 'null'])
    assert_valid s, 'x'
    assert_valid s, nil
    refute_valid s, 0
    refute_valid s, []
  end

  def test_empty_schema_accepts_anything
    s = schema({})
    assert_valid s, 0
    assert_valid s, 'x'
    assert_valid s, [1, 2]
    assert_valid s, nil
    assert_valid s, {'a' => 1}
  end

  def test_true_schema
    s = schema(true)
    assert_valid s, 0
    assert_valid s, 'x'
  end

  def test_false_schema
    s = schema(false)
    refute_valid s, 0
    refute_valid s, 'x'
    refute_valid s, nil
  end
end
