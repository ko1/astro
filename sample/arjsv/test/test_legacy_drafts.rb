require_relative 'test_helper'

class TestLegacyDrafts < ArjsvTest
  # draft-04 used bare `id` instead of `$id` for identifier nodes.  arjsv
  # accepts both as equivalent so legacy schemas continue to work.
  def test_draft04_id_based_ref
    s = schema(
      '$schema'  => 'http://json-schema.org/draft-04/schema#',
      'definitions' => {
        'PositiveInt' => {'id' => 'positive-int', 'type' => 'integer', 'minimum' => 0},
      },
      '$ref' => 'positive-int',
    )
    assert_valid s, 5
    refute_valid s, -1
    refute_valid s, 'x'
  end

  # draft-04 / draft-06 used Boolean `exclusiveMinimum` paired with
  # `minimum` (exclusive: true means "use exclusive form").  Already
  # supported in `lower_minimum` / `lower_maximum`.
  def test_draft04_boolean_exclusive_minimum
    s = schema('minimum' => 0, 'exclusiveMinimum' => true)
    refute_valid s, 0
    assert_valid s, 0.0001
    assert_valid s, 1
  end

  def test_draft04_boolean_exclusive_maximum
    s = schema('maximum' => 10, 'exclusiveMaximum' => true)
    refute_valid s, 10
    assert_valid s, 9.9999
  end

  # `definitions` (draft-04 / 06 / 07) is the alias for `$defs` (2019-09+).
  def test_definitions_alias
    s = schema(
      'definitions' => {'NonEmpty' => {'type' => 'string', 'minLength' => 1}},
      '$ref' => '#/definitions/NonEmpty',
    )
    assert_valid s, 'x'
    refute_valid s, ''
    refute_valid s, 0
  end
end
