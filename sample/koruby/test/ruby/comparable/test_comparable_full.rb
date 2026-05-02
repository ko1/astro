require_relative "../../test_helper"

# Verify Comparable derives the full comparison protocol from <=>
# on user-defined classes.

class Score
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(other)
    return nil unless other.is_a?(Score)
    @n <=> other.n
  end
end

S5  = Score.new(5)
S10 = Score.new(10)
S15 = Score.new(15)

def test_ge_via_cmp
  assert_equal true,  S10 >= S10
  assert_equal true,  S15 >= S10
  assert_equal false, S5  >= S10
end

def test_clamp_two_args
  # clamp(min, max) — uses <=> against both bounds.
  assert_equal S10, S5.clamp(S10, S15)
  assert_equal S10, S15.clamp(S5, S10)
  assert_equal S10, S10.clamp(S5, S15)
end

def test_clamp_with_range
  # clamp(min..max) — half-bounded supported by passing nil bound via Range.
  r = S10..S15
  assert_equal S10, S5.clamp(r)
  assert_equal S15, Score.new(99).clamp(r)
  assert_equal S10, S10.clamp(r)
end

def test_eq_with_incompatible
  # <=> returns nil for non-Score → == should be false, not raise.
  assert_equal false, S10 == "ten"
  assert_equal false, S10 == 10
end

def test_lt_raises_on_incompatible
  # Comparable raises ArgumentError when <=> returns nil for ordering ops.
  raised = false
  begin
    S10 < "ten"
  rescue ArgumentError
    raised = true
  end
  assert_equal true, raised
end

def test_chain_with_minmax
  scores = [S15, S5, S10]
  assert_equal S5,  scores.min
  assert_equal S15, scores.max
  assert_equal [S5, S15], scores.minmax
end

def test_sort_by
  scores = [S15, S5, S10]
  assert_equal [S5, S10, S15], scores.sort_by { |s| s.n }
end

TESTS = [
  :test_ge_via_cmp,
  :test_clamp_two_args,
  :test_clamp_with_range,
  :test_eq_with_incompatible,
  :test_lt_raises_on_incompatible,
  :test_chain_with_minmax,
  :test_sort_by,
]
TESTS.each { |t| run_test(t) }
report "ComparableFull"
