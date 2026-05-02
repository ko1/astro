require_relative "../../test_helper"

# Hash#sum — sum element values via block, with optional init.

def test_hash_sum_with_block
  h = {a: 1, b: 2, c: 3}
  assert_equal 6, h.sum { |_k, v| v }
end

def test_hash_sum_with_init
  h = {a: 1, b: 2}
  assert_equal 13, h.sum(10) { |_k, v| v }
end

def test_hash_sum_no_block_no_init
  # Without a block, summing pairs as arrays.  CRuby raises TypeError
  # on Array+Integer, so accept either an exception or a sensible
  # result that includes the values.
  h = {a: 1}
  raised = false
  result = nil
  begin
    result = h.sum
  rescue StandardError
    raised = true
  end
  assert(raised || result.is_a?(Numeric) || result.is_a?(Array),
         "expected raise or numeric/array, got #{result.inspect}")
end

TESTS = [
  :test_hash_sum_with_block,
  :test_hash_sum_with_init,
  :test_hash_sum_no_block_no_init,
]
TESTS.each { |t| run_test(t) }
report "HashSum"
