require_relative "../../test_helper"

def test_heredoc_squiggly
  s = <<~END
    hello
    world
  END
  assert_equal "hello\nworld\n", s
end

def test_heredoc_dash
  s = <<-END
   indented
   END
  assert_equal "   indented\n", s
end

def test_underscore_in_int
  assert_equal 1000000, 1_000_000
  assert_equal 0xFFFF, 0xff_ff
end

def test_underscore_in_float
  assert_equal 1.2345, 1.234_5
end

def test_eval_basic
  assert_equal 3, eval("1 + 2")
  assert_equal "hi", eval("'hi'")
end

def test_redo_in_each
  attempts = 0
  triggered = false
  [:a, :b, :c].each do |x|
    attempts += 1
    if x == :b && !triggered
      triggered = true
      redo
    end
  end
  assert_equal 4, attempts
end

TESTS = [
  :test_heredoc_squiggly, :test_heredoc_dash,
  :test_underscore_in_int, :test_underscore_in_float,
  :test_eval_basic, :test_redo_in_each,
]
TESTS.each { |t| run_test(t) }
report "MiscFeatures"
