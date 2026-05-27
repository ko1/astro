require_relative "../../test_helper"

# method_missing / respond_to? semantics.

class MMRecorder
  def initialize
    @calls = []
  end
  attr_reader :calls
  def method_missing(name, *args)
    @calls << [name, args]
    "fallback:#{name}:#{args.join(",")}"
  end
end

def test_method_missing_caught
  r = MMRecorder.new
  assert_equal "fallback:foo:1,2", r.foo(1, 2)
  assert_equal [[:foo, [1, 2]]], r.calls
end

def test_method_missing_chained_calls
  r = MMRecorder.new
  r.a
  r.b(1)
  r.c(:x, :y, :z)
  assert_equal [[:a, []], [:b, [1]], [:c, [:x, :y, :z]]], r.calls
end

# ---------- respond_to_missing? ----------

class RTM
  def respond_to_missing?(name, include_private = false)
    name == :magic
  end
  def method_missing(name, *args)
    return :ok if name == :magic
    super
  end
end

def test_respond_to_uses_respond_to_missing
  o = RTM.new
  assert_equal true,  o.respond_to?(:magic)
  assert_equal false, o.respond_to?(:nonexistent)
  assert_equal :ok,   o.magic
end

# ---------- BasicObject does NOT have method_missing fallback by default ----------
# (We only test that defined object's MM fires; calling on something
# without MM should raise NoMethodError or RuntimeError.)

def test_no_method_raises
  raised = false
  begin
    Object.new.totally_nonexistent_method
  rescue NoMethodError, RuntimeError
    raised = true
  end
  assert raised, "expected NoMethodError or RuntimeError"
end

TESTS = [
  :test_method_missing_caught,
  :test_method_missing_chained_calls,
  :test_respond_to_uses_respond_to_missing,
  :test_no_method_raises,
]
TESTS.each { |t| run_test(t) }
report "MethodMissing"
