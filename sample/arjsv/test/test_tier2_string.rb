require_relative 'test_helper'

class TestTier2String < ArjsvTest
  def test_pattern
    s = schema('pattern' => '^[a-z]+$')
    assert_valid s, 'hello'
    assert_valid s, 'a'
    refute_valid s, 'Hello'
    refute_valid s, 'hello world'
    refute_valid s, ''
  end

  def test_pattern_skips_non_string
    s = schema('pattern' => '^[a-z]+$')
    assert_valid s, 0
    assert_valid s, []
    assert_valid s, nil
  end

  def test_pattern_with_type_constraint
    s = schema('type' => 'string', 'pattern' => '\A\d{4}-\d{2}-\d{2}\z')
    assert_valid s, '2024-01-15'
    refute_valid s, '24-1-15'
    refute_valid s, 12345
  end

  def test_format_email
    s = schema('format' => 'email')
    assert_valid s, 'a@b.co'
    assert_valid s, 'first.last@example.com'
    assert_valid s, '"quoted local"@example.com'
    assert_valid s, 'user@[127.0.0.1]'
    refute_valid s, 'no-at'
    refute_valid s, '@example.com'                # missing local part
    refute_valid s, 'user@'                        # missing domain
    refute_valid s, 'user@invalid=domain.com'      # `=` in label
    refute_valid s, '.user@example.com'            # leading dot in local
    refute_valid s, 'user..name@example.com'       # consecutive dots
  end

  def test_format_date
    s = schema('format' => 'date')
    assert_valid s, '2024-01-15'
    refute_valid s, '2024/01/15'
    refute_valid s, '24-01-15'
  end

  def test_format_date_time
    s = schema('format' => 'date-time')
    assert_valid s, '2024-01-15T10:30:00Z'
    assert_valid s, '2024-01-15T10:30:00+09:00'
    assert_valid s, '2024-01-15T10:30:00.123Z'
    refute_valid s, '2024-01-15'
    refute_valid s, '2024-01-15 10:30'
  end

  def test_format_uuid
    s = schema('format' => 'uuid')
    assert_valid s, '550e8400-e29b-41d4-a716-446655440000'
    refute_valid s, '550e8400-e29b-41d4-a716'
    refute_valid s, 'not-a-uuid'
  end

  def test_format_unknown_passes
    # Unknown format strings are annotation-only per draft-07.
    s = schema('format' => 'made-up-format')
    assert_valid s, 'anything'
    assert_valid s, 12345
  end
end
