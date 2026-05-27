require_relative "../../test_helper"

# Heredoc string literal forms.

def test_basic_heredoc
  s = <<~HEREDOC
    line1
    line2
  HEREDOC
  assert_equal "line1\nline2\n", s
end

def test_heredoc_no_interp_squiggly
  v = "world"
  s = <<~HEREDOC
    hello #{v}
  HEREDOC
  assert_equal "hello world\n", s
end

def test_heredoc_singlequote_no_interp
  v = "world"
  s = <<~'HEREDOC'
    hello #{v}
  HEREDOC
  # Single-quoted heredoc should keep \#{v} literal.
  assert_equal "hello \#{v}\n", s
end

def test_unindented_heredoc
  s = <<-END
    indented
  END
  # <<- preserves leading whitespace; <<~ strips common indent.
  assert_equal "    indented\n", s
end

def test_heredoc_in_array
  arr = [<<~A, <<~B]
    one
  A
    two
  B
  assert_equal "one\n", arr[0]
  assert_equal "two\n", arr[1]
end

TESTS = [
  :test_basic_heredoc,
  :test_heredoc_no_interp_squiggly,
  :test_heredoc_singlequote_no_interp,
  :test_unindented_heredoc,
  :test_heredoc_in_array,
]
TESTS.each { |t| run_test(t) }
report "Heredoc"
