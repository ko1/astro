require_relative 'test_helper'

class TestTier2Combinators < ArjsvTest
  def test_all_of
    s = schema('allOf' => [
      {'type' => 'integer'},
      {'minimum' => 0},
      {'maximum' => 100},
    ])
    assert_valid s, 50
    assert_valid s, 0
    assert_valid s, 100
    refute_valid s, -1
    refute_valid s, 101
    refute_valid s, 'x'
  end

  def test_any_of
    s = schema('anyOf' => [
      {'type' => 'string'},
      {'type' => 'null'},
    ])
    assert_valid s, 'x'
    assert_valid s, nil
    refute_valid s, 0
    refute_valid s, []
  end

  def test_one_of
    s = schema('oneOf' => [
      {'type' => 'integer', 'multipleOf' => 5},
      {'type' => 'integer', 'multipleOf' => 3},
    ])
    assert_valid s, 5             # only multipleOf 5
    assert_valid s, 3             # only multipleOf 3
    refute_valid s, 15            # both multipleOf 5 AND 3 → 2 matches → fail
    refute_valid s, 7             # neither → 0 matches → fail
  end

  def test_one_of_with_objects
    s = schema('oneOf' => [
      {'type' => 'object', 'required' => ['name']},
      {'type' => 'object', 'required' => ['id']},
    ])
    assert_valid s, {'name' => 'X'}                  # 1 match
    assert_valid s, {'id' => 1}                      # 1 match
    refute_valid s, {'name' => 'X', 'id' => 1}       # 2 matches
    refute_valid s, {}                               # 0 matches
  end

  def test_not
    s = schema('not' => {'type' => 'string'})
    assert_valid s, 0
    assert_valid s, nil
    assert_valid s, []
    refute_valid s, 'x'
  end

  def test_not_combined
    s = schema(
      'type' => 'integer',
      'not' => {'minimum' => 100},
    )
    assert_valid s, 0
    assert_valid s, 99
    refute_valid s, 100
    refute_valid s, 'x'
  end

  def test_if_then_else
    s = schema(
      'if' => {'type' => 'integer', 'minimum' => 0},
      'then' => {'maximum' => 100},
      'else' => {'type' => 'string'},
    )
    assert_valid s, 50              # if matches, then check minimum (yes, ≤100)
    assert_valid s, 'hello'         # if fails, else is string check
    refute_valid s, 200             # if matches, then fails
    refute_valid s, []              # if fails, else fails (not a string)
    refute_valid s, -5              # if fails (minimum 0), else fails
  end

  def test_if_then_no_else
    s = schema(
      'if' => {'type' => 'integer'},
      'then' => {'minimum' => 0},
    )
    assert_valid s, 50              # if matches, then ok
    assert_valid s, 'whatever'      # if fails, no else = pass
    refute_valid s, -1              # if matches, then fails
  end
end
