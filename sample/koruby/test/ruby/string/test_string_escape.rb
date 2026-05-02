require_relative "../../test_helper"

# String literal escape sequences and interpolation.

def test_basic_escapes
  assert_equal 5, "hello".length
  assert_equal "a\nb".length, 3
  assert_equal "\t".bytes, [9]
  assert_equal "\\".length, 1
end

def test_double_quoted_escape_codes
  assert_equal 0,   "\0".bytes[0]
  assert_equal 7,   "\a".bytes[0]      # \a → BEL
  assert_equal 8,   "\b".bytes[0]      # \b → BS
  assert_equal 27,  "\e".bytes[0]      # \e → ESC
  assert_equal 12,  "\f".bytes[0]      # \f → FF
  assert_equal 11,  "\v".bytes[0]      # \v → VT
end

def test_hex_escape
  assert_equal 0xff, "\xff".bytes[0]
  assert_equal 0x41, "\x41".bytes[0]
end

def test_octal_escape
  assert_equal 0o101, "\101".bytes[0]
  assert_equal 0,     "\0".bytes[0]
end

def test_single_quoted_no_escape
  # Single-quoted: only \\ and \' are special; \n stays literal.
  assert_equal 2, '\n'.length            # backslash + n
  assert_equal "\\", '\\'                # one backslash
end

# ---------- string interpolation ----------

def test_interpolation_simple
  x = 42
  assert_equal "x=42", "x=#{x}"
end

def test_interpolation_expression
  assert_equal "3", "#{1 + 2}"
end

def test_interpolation_method_call
  s = "hello"
  assert_equal "len=5", "len=#{s.length}"
end

def test_interpolation_nested
  assert_equal "(a)(b)", "#{"(#{:a})"}#{"(#{:b})"}"
end

TESTS = [
  :test_basic_escapes,
  :test_double_quoted_escape_codes,
  :test_hex_escape,
  :test_octal_escape,
  :test_single_quoted_no_escape,
  :test_interpolation_simple,
  :test_interpolation_expression,
  :test_interpolation_method_call,
  :test_interpolation_nested,
]
TESTS.each { |t| run_test(t) }
report "StringEscape"
