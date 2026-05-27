require_relative "../../test_helper"

def collect(*args)
  args
end

def fwd_simple(...)
  collect(...)
end

def test_forward_positional
  assert_equal [1, 2, 3], fwd_simple(1, 2, 3)
end

def test_forward_no_args
  assert_equal [], fwd_simple
end

def collect_kw(*args, **opts)
  [args, opts]
end

def fwd_kw(...)
  collect_kw(...)
end

def test_forward_with_kwargs
  assert_equal [[1, 2], {a: 3, b: 4}], fwd_kw(1, 2, a: 3, b: 4)
end

def test_forward_kw_only
  assert_equal [[], {x: 1}], fwd_kw(x: 1)
end

def collect_with_block(a, &blk)
  [a, blk.call]
end

def fwd_block(...)
  collect_with_block(...)
end

def test_forward_block
  assert_equal [10, 99], fwd_block(10) { 99 }
end

TESTS = [
  :test_forward_positional, :test_forward_no_args,
  :test_forward_with_kwargs, :test_forward_kw_only,
  :test_forward_block,
]
TESTS.each { |t| run_test(t) }
report "Forwarding"
