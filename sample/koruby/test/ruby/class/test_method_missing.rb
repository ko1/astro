# Tests for method_missing — caller-supplied fallback.

require_relative "../../test_helper"

class Tracer
  def method_missing(name, *args)
    "called #{name}(#{args.join(',')})"
  end
end

def test_method_missing_basic
  t = Tracer.new
  assert_equal "called foo()", t.foo
end

def test_method_missing_with_args
  t = Tracer.new
  assert_equal "called bar(1,2,3)", t.bar(1, 2, 3)
end

# method_missing chained through inheritance
class Base
  def method_missing(name, *args)
    [:base, name]
  end
end

class Derived < Base; end

def test_method_missing_inherited
  d = Derived.new
  assert_equal [:base, :anything], d.anything
end

# Real method takes precedence over method_missing
class Real
  def known; :known; end
  def method_missing(name, *args); :missing; end
end

def test_real_method_takes_precedence
  r = Real.new
  assert_equal :known, r.known
  assert_equal :missing, r.unknown
end

# respond_to? returns false for method_missing-handled names by default
def test_respond_to_false_for_missing
  t = Tracer.new
  assert_equal false, t.respond_to?(:foo)
end

TESTS = [
  :test_method_missing_basic,
  :test_method_missing_with_args,
  :test_method_missing_inherited,
  :test_real_method_takes_precedence,
  :test_respond_to_false_for_missing,
]

TESTS.each { |t| run_test(t) }
report "MethodMissing"
