require_relative "../../test_helper"

# Backtrace line numbers for cfunc raises — make sure raise from a
# cfunc reports the source line where the cfunc was called, and that
# the chain walks each frame's actual call site.

def helper_raise
  raise "boom"            # line 8
end

def test_simple_raise_line
  begin
    helper_raise          # line 13
  rescue => e
    bt = e.backtrace
    assert(bt.size >= 2, "backtrace too short")
    assert(bt[0].include?(":8:in `helper_raise'"), "first frame: #{bt[0]}")
  end
end

def helper_div_zero
  10 / 0                  # line 22
end

def test_divide_by_zero_line
  begin
    helper_div_zero
  rescue => e
    bt = e.backtrace
    assert(bt[0].include?(":22:in `helper_div_zero'"), "div frame: #{bt[0]}")
  end
end

def helper_arg_error
  [].first(-1)            # line 35
end

def test_cfunc_raise_line
  begin
    helper_arg_error
  rescue => e
    bt = e.backtrace
    assert(bt[0].include?(":35:in `helper_arg_error'"), "cfunc frame: #{bt[0]}")
  end
end

def deeper_inner
  raise "deep"            # line 48
end
def deeper_middle
  deeper_inner            # line 51
end
def deeper_outer
  deeper_middle           # line 54
end

def test_deep_chain
  begin
    deeper_outer
  rescue => e
    bt = e.backtrace
    assert(bt[0].include?(":48:in `deeper_inner'"),  "inner: #{bt[0]}")
    assert(bt[1].include?(":51:in `deeper_middle'"), "middle: #{bt[1]}")
    assert(bt[2].include?(":54:in `deeper_outer'"),  "outer: #{bt[2]}")
  end
end

# Method#source_location should still report the def's line, not the
# first statement's line.
def some_method                # line 70
  x = 1
  x + 1
end

def test_source_location_unchanged
  loc = method(:some_method).source_location
  assert_equal 70, loc[1]
end

TESTS = %i[
  test_simple_raise_line test_divide_by_zero_line
  test_cfunc_raise_line test_deep_chain
  test_source_location_unchanged
]
TESTS.each {|t| run_test(t) }
report "BacktraceLines"
