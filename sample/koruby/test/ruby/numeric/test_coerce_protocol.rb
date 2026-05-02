require_relative "../../test_helper"

# Numeric#coerce protocol: `1 + my_num` invokes my_num.coerce(1)
# when my_num isn't a built-in numeric.

class Money
  attr_reader :amount
  def initialize(a); @amount = a; end
  def coerce(other); [Money.new(other), self]; end
  def +(other); Money.new(@amount + other.amount); end
  def -(other); Money.new(@amount - other.amount); end
  def *(other); Money.new(@amount * other.amount); end
  def ==(other); other.is_a?(Money) && other.amount == @amount; end
end

def test_coerce_plus_int_first
  result = 10 + Money.new(100)
  assert_equal Money.new(110), result
end

def test_coerce_minus_int_first
  result = 100 - Money.new(30)
  assert_equal Money.new(70), result
end

def test_coerce_mul_int_first
  result = 5 * Money.new(20)
  assert_equal Money.new(100), result
end

def test_coerce_no_coerce_method_raises
  raised = false
  begin
    1 + Object.new
  rescue StandardError
    raised = true
  end
  assert raised, "expected error when RHS has no coerce"
end

# ---------- coerce returning bad pair raises ----------

class BadCoerce
  def coerce(other); :not_an_array; end
end

def test_coerce_bad_return_raises
  raised = false
  begin
    1 + BadCoerce.new
  rescue StandardError
    raised = true
  end
  assert raised, "expected error when coerce returns non-Array"
end

TESTS = [
  :test_coerce_plus_int_first,
  :test_coerce_minus_int_first,
  :test_coerce_mul_int_first,
  :test_coerce_no_coerce_method_raises,
  :test_coerce_bad_return_raises,
]
TESTS.each { |t| run_test(t) }
report "CoerceProtocol"
