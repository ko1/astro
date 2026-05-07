require_relative 'test_helper'

class TestString < ArjsvTest
  def test_min_length
    s = schema('minLength' => 3)
    assert_valid s, 'abc'
    assert_valid s, 'abcd'
    refute_valid s, 'ab'
    refute_valid s, ''
  end

  def test_max_length
    s = schema('maxLength' => 3)
    assert_valid s, ''
    assert_valid s, 'abc'
    refute_valid s, 'abcd'
  end

  def test_length_for_non_string_skipped
    s = schema('minLength' => 3)
    assert_valid s, 0
    assert_valid s, []
    assert_valid s, nil
  end

  def test_length_unicode_counts_characters
    # rb_str_strlen counts characters per encoding.  These three are 3 chars each.
    s = schema('type' => 'string', 'minLength' => 3, 'maxLength' => 3)
    assert_valid s, 'abc'
    assert_valid s, 'あいう'
    refute_valid s, 'ab'
    refute_valid s, 'abcd'
  end

  def test_length_combined_with_type
    s = schema('type' => 'string', 'minLength' => 1, 'maxLength' => 5)
    assert_valid s, 'a'
    assert_valid s, 'hello'
    refute_valid s, ''
    refute_valid s, 'hello!'
    refute_valid s, 42
  end
end
