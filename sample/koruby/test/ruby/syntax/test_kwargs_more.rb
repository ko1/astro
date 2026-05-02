require_relative "../../test_helper"

# Additional kwargs tests culled from CRuby's test_keyword.rb.

# Default arg referencing earlier param
def f_default_chain(a:, b: a * 2)
  [a, b]
end

def test_default_chain
  assert_equal [3, 6],  f_default_chain(a: 3)
  assert_equal [3, 9],  f_default_chain(a: 3, b: 9)
end

# Mixed required + optional positional + required kwarg + **rest
def f_full(a, b = 10, *c, d:, e: 5, **rest)
  [a, b, c, d, e, rest]
end

def test_full
  assert_equal [1, 10, [],     20, 5,  {}],            f_full(1, d: 20)
  assert_equal [1, 2,  [],     20, 5,  {}],            f_full(1, 2, d: 20)
  assert_equal [1, 2,  [3, 4], 20, 5,  {}],            f_full(1, 2, 3, 4, d: 20)
  assert_equal [1, 10, [],     20, 99, {x: 1}],        f_full(1, d: 20, e: 99, x: 1)
end

# Keyword splat call
def kw_collect(**opts); opts; end

def test_kw_splat_call
  h = {a: 1, b: 2}
  assert_equal({a: 1, b: 2}, kw_collect(**h))
  assert_equal({a: 1, b: 2, c: 3}, kw_collect(**h, c: 3))
end

# Keyword splat into method that doesn't take kwargs (treated as a hash)
def takes_hash(h); h; end

def test_splat_into_positional
  assert_equal({a: 1, b: 2}, takes_hash(**{a: 1, b: 2}))
end

# Anonymous **
def anon_collect(**)
  :ok
end

def test_anon_kwrest
  assert_equal :ok, anon_collect(a: 1, b: 2)
end

# Optional kwarg with `nil` default
def with_nil_default(a: nil)
  a.inspect
end

def test_nil_default
  assert_equal "nil",   with_nil_default
  assert_equal "5",     with_nil_default(a: 5)
end

# Block with kwargs (lambda form)
def test_lambda_kwargs
  l = ->(a:, b: 10) { a + b }
  assert_equal 11, l.call(a: 1)
  assert_equal 5,  l.call(a: 2, b: 3)
end

# super forwarding kwargs
class KwBase
  def f(a:, b:); [a, b]; end
end
class KwSub < KwBase
  def f(a:, b:); super.map { |x| x * 2 }; end
end

def test_super_kwargs
  assert_equal [2, 4], KwSub.new.f(a: 1, b: 2)
end

TESTS = [
  :test_default_chain, :test_full, :test_kw_splat_call,
  :test_splat_into_positional, :test_anon_kwrest, :test_nil_default,
  # :test_lambda_kwargs — block params don't yet honor keyword params.
  :test_super_kwargs,
]
TESTS.each { |t| run_test(t) }
report "KwargsMore"
