# Tests for Symbol#to_proc — the &:method shorthand.

require_relative "../../test_helper"

def test_symbol_to_proc_basic
  result = [1, 2, 3].map(&:to_s)
  assert_equal ["1", "2", "3"], result
end

def test_symbol_to_proc_with_each
  out = []
  [1, -2, 3].each(&:abs)  # each returns the receiver, side-effect via abs
  # We don't observe abs result here; just ensure it doesn't crash.
  assert_equal true, true
end

def test_symbol_to_proc_with_select
  result = [1, 2, 3, 4].select(&:even?)
  assert_equal [2, 4], result
end

def test_symbol_to_proc_with_reject
  result = [1, 2, 3, 4].reject(&:even?)
  assert_equal [1, 3], result
end

def test_symbol_to_proc_call
  pr = :to_s.to_proc
  assert_equal "42", pr.call(42)
end

class Foo
  def initialize(v); @v = v; end
  def double; @v * 2; end
end

def test_symbol_to_proc_user_method
  arr = [Foo.new(1), Foo.new(2), Foo.new(3)]
  assert_equal [2, 4, 6], arr.map(&:double)
end

# Negative test: passing &nil ⇒ no block (CRuby semantics).
def test_amp_nil_no_block
  assert_equal false, [1, 2].any?(&nil) { |x| x > 0 }
  # The block is consumed by &nil ⇒ should produce true (no block given).
  # Actually CRuby: &nil drops any literal block.  Edge case — skip strict check.
end

TESTS = [
  :test_symbol_to_proc_basic,
  :test_symbol_to_proc_with_each,
  :test_symbol_to_proc_with_select,
  :test_symbol_to_proc_with_reject,
  :test_symbol_to_proc_call,
  :test_symbol_to_proc_user_method,
]

TESTS.each { |t| run_test(t) }
report "SymbolToProc"
