require_relative "../../test_helper"

# Required kwargs
def kwfn(a:, b:)
  [a, b]
end

def test_required_kwargs
  assert_equal [1, 2], kwfn(a: 1, b: 2)
  assert_equal [1, 2], kwfn(b: 2, a: 1)
end

# Optional kwargs (with defaults)
def kwdef(a: 10, b: 20)
  a + b
end

def test_optional_kwargs_defaults
  assert_equal 30, kwdef
  assert_equal 25, kwdef(a: 5)
  assert_equal 50, kwdef(a: 30, b: 20)
end

# Mixed: positional + required kwarg
def mixed(x, y:)
  x + y
end

def test_mixed_positional_and_kwarg
  assert_equal 12, mixed(2, y: 10)
end

# Mixed: positional + optional kwarg
def mixed_opt(x, y: 100)
  x + y
end

def test_mixed_positional_and_optional_kwarg
  assert_equal 105, mixed_opt(5)
  assert_equal 12, mixed_opt(2, y: 10)
end

# **opts splat at the call site
def take_a_b(a:, b:); [a, b]; end

def test_double_splat_call
  opts = {a: 1, b: 2}
  assert_equal [1, 2], take_a_b(**opts)
end

def test_double_splat_with_extra_kw
  opts = {a: 1}
  assert_equal [1, 99], take_a_b(**opts, b: 99)
end

# **kwrest receive
def collect_kwargs(**opts); opts; end

def test_kwrest_receive
  assert_equal({a: 1, b: 2}, collect_kwargs(a: 1, b: 2))
  assert_equal({}, collect_kwargs)
end

# Mixed: required positional + required kwarg + **rest
def mixed_kwrest(x, a:, **rest)
  [x, a, rest]
end

def test_mixed_pos_kw_kwrest
  assert_equal [1, 2, {c: 3, d: 4}], mixed_kwrest(1, a: 2, c: 3, d: 4)
end

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

TESTS = %i[
  test_required_kwargs test_optional_kwargs_defaults test_mixed_positional_and_kwarg test_mixed_positional_and_optional_kwarg
  test_double_splat_call test_double_splat_with_extra_kw test_kwrest_receive test_mixed_pos_kw_kwrest
  test_default_chain test_full test_kw_splat_call test_splat_into_positional
  test_anon_kwrest test_nil_default test_lambda_kwargs test_super_kwargs
]
TESTS.each {|t| run_test(t) }
report "Kwargs"
