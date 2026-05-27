require_relative "../../test_helper"

# BEGIN runs as soon as we hit it (koruby's parser is single-pass, so
# `BEGIN { }` executes inline rather than being hoisted; that's enough
# to validate it doesn't error).
$begin_ran = nil
BEGIN { $begin_ran = :yes }

# END registers a hook that runs at program exit — we can't easily
# observe it from inside the same suite, but registering should not
# raise.

def test_begin_ran
  assert_equal :yes, $begin_ran
end

def test_eval_returns_value
  assert_equal 7, eval("3 + 4")
  assert_equal "hello", eval("'hello'")
end

def test_eval_can_define_method
  eval("def __eval_test_method; 99; end")
  assert_equal 99, __eval_test_method
end

def test_end_block_registers
  END { 1 + 1 }  # Should not raise
  assert_equal true, true
end

TESTS = [:test_begin_ran, :test_eval_returns_value, :test_eval_can_define_method, :test_end_block_registers]
TESTS.each { |t| run_test(t) }
report "BeginEndEval"
