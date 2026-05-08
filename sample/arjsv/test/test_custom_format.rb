require_relative 'test_helper'

class TestCustomFormat < ArjsvTest
  # User-defined formats via `formats:` constructor option.  Mirrors
  # json_schemer's API.
  def test_user_defined_format
    s = Arjsv.schema(
      {'type' => 'string', 'format' => 'phone'},
      formats: {'phone' => ->(v) { v =~ /\A\+?\d{10,15}\z/ }},
    )
    assert s.valid?('+819012345678')
    assert s.valid?('0312345678')
    refute s.valid?('not-a-phone')
    refute s.valid?('123')                     # too short
    refute s.valid?(42)                        # type:string fails
  end

  def test_user_defined_format_skips_non_strings
    # Without `type: 'string'`, the format check applies only to strings
    # and is a no-op for everything else.
    s = Arjsv.schema(
      {'format' => 'phone'},
      formats: {'phone' => ->(v) { v =~ /\A\d+\z/ }},
    )
    assert s.valid?('1234')
    refute s.valid?('not-digits')
    assert s.valid?(42)                        # non-string → format skipped
    assert s.valid?(nil)
    assert s.valid?([])
  end

  def test_user_format_overrides_builtin
    # `email` is built-in; user override wins.
    strict_email = ->(v) { v =~ /\A[a-z]+@example\.com\z/ }
    s = Arjsv.schema(
      {'type' => 'string', 'format' => 'email'},
      formats: {'email' => strict_email},
    )
    assert s.valid?('alice@example.com')
    refute s.valid?('alice@other.com')         # built-in would accept this
  end

  def test_user_format_disabled
    # A `nil` or `false` mapping disables the format → annotation-only.
    s = Arjsv.schema(
      {'type' => 'string', 'format' => 'email'},
      formats: {'email' => false},
    )
    assert s.valid?('not-an-email')            # disabled → no constraint
  end

  def test_unknown_user_format_falls_back_to_pass
    s = Arjsv.schema(
      {'type' => 'string', 'format' => 'made-up'},
    )
    # Unknown format: annotation-only per spec.
    assert s.valid?('anything')
  end

  def test_symbol_keyed_user_formats
    s = Arjsv.schema(
      {'type' => 'string', 'format' => 'short'},
      formats: {short: ->(v) { v.length <= 3 }},
    )
    assert s.valid?('abc')
    refute s.valid?('abcd')
  end
end
