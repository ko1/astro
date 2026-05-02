# Tests for `&blk` block parameter (block reified as Proc).

require_relative "../../test_helper"

# --- basic &blk reification --------------------------------------

def takes_block(&blk)
  blk
end

def test_block_arg_is_proc
  proc_val = takes_block { |x| x * 2 }
  assert_equal Proc, proc_val.class
  assert_equal 6, proc_val.call(3)
end

def test_no_block_blk_is_nil
  result = takes_block
  assert_equal nil, result
end

# --- mix &blk with positional args -------------------------------

def with_args(a, b, &blk)
  blk ? blk.call(a + b) : (a + b)
end

def test_mixed_args_no_block
  assert_equal 5, with_args(2, 3)
end

def test_mixed_args_with_block
  assert_equal 50, with_args(2, 3) { |x| x * 10 }
end

# --- &blk forwarding ----------------------------------------------

def yielder(&blk)
  blk.call(1) + blk.call(2) + blk.call(3)
end

def test_blk_call_multiple
  assert_equal 60, yielder { |x| x * 10 }
end

# --- block_given? + &blk ------------------------------------------

def maybe(&blk)
  if blk
    blk.call(42)
  else
    -1
  end
end

def test_blk_truthy_when_given
  assert_equal 84, maybe { |x| x * 2 }
end

def test_blk_falsy_when_absent
  assert_equal -1, maybe
end

TESTS = [
  :test_block_arg_is_proc, :test_no_block_blk_is_nil,
  :test_mixed_args_no_block, :test_mixed_args_with_block,
  :test_blk_call_multiple,
  :test_blk_truthy_when_given, :test_blk_falsy_when_absent,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK BlockArg (#{$pass})"
else
  puts "FAIL BlockArg: #{$fail}/#{$pass + $fail}"
end
