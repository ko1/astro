require_relative "../../test_helper"

# Heredoc edge cases beyond the basic forms (test_heredoc.rb covers
# those): multiple on a line, interpolation w/ method call, nested.

def test_two_heredocs_same_line
  arr = [<<~A, <<~B]
    one
    two
  A
    three
    four
  B
  assert_equal "one\ntwo\n", arr[0]
  assert_equal "three\nfour\n", arr[1]
end

def test_heredoc_as_method_arg
  def take_str(s); s.length; end
  result = take_str(<<~END)
    hello
  END
  assert_equal 6, result
end

def test_heredoc_method_call_in_interp
  v = 5
  s = <<~MSG
    v=#{v}, doubled=#{v * 2}, up=#{"hi".upcase}
  MSG
  assert_equal "v=5, doubled=10, up=HI\n", s
end

def test_heredoc_nested_in_interp
  outer = <<~OUTER
    start
    #{
      inner = <<~INNER
        nested
      INNER
      inner
    }end
  OUTER
  assert outer.include?("nested"), "got #{outer.inspect}"
  assert outer.include?("start"), "got #{outer.inspect}"
end

def test_heredoc_dash_keeps_indent
  a = <<-DASH
    indented
  DASH
  assert_equal "    indented\n", a
end

def test_heredoc_squiggly_strips_indent
  b = <<~SQUIG
    flush
  SQUIG
  assert_equal "flush\n", b
end

# ---------- single-quoted heredoc — no interp ----------

def test_heredoc_single_quoted
  v = "ignore"
  s = <<~'PLAIN'
    no #{v} here
  PLAIN
  assert_equal "no \#{v} here\n", s
end

TESTS = [
  :test_two_heredocs_same_line,
  :test_heredoc_as_method_arg,
  :test_heredoc_method_call_in_interp,
  :test_heredoc_nested_in_interp,
  :test_heredoc_dash_keeps_indent,
  :test_heredoc_squiggly_strips_indent,
  :test_heredoc_single_quoted,
]
TESTS.each { |t| run_test(t) }
report "HeredocMore"
