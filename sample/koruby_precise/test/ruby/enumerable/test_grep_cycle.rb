require_relative "../../test_helper"

# Enumerable#grep / grep_v / find_index / cycle — added 2026-05-02 to
# the *first* `class Enumerable` block so they propagate through
# include-time snapshot to Array / Range / Hash.

# ---------- grep / grep_v ----------

def test_grep_with_range
  assert_equal [2, 3], [1, 2, 3, 4].grep(2..3)
end

def test_grep_with_class
  assert_equal [1, 3], [1, "two", 3].grep(Integer)
end

def test_grep_with_block_transforms
  assert_equal [4, 9], [1, 2, 3].grep(2..3) { |x| x * x }
end

def test_grep_v
  assert_equal ["two"], [1, "two", 3].grep_v(Integer)
end

def test_grep_on_range
  # Range includes Enumerable too.
  assert_equal [2, 3, 4], (1..5).grep(2..4)
end

# ---------- find_index ----------

def test_find_index_target
  assert_equal 1, ["a", "b", "c"].find_index("b")
end

def test_find_index_block
  assert_equal 2, [1, 2, 3, 4].find_index { |x| x > 2 }
end

def test_find_index_miss
  assert_equal nil, [1, 2, 3].find_index(99)
end

# ---------- cycle ----------

def test_cycle_with_block_n_times
  out = []
  [1, 2, 3].cycle(2) { |x| out << x }
  assert_equal [1, 2, 3, 1, 2, 3], out
end

def test_cycle_first_n_no_block
  # No block + .first(N): koruby materializes 100 cycles which is
  # enough for any reasonable .first call.
  assert_equal [1, 2, 3, 1, 2, 3, 1], [1, 2, 3].cycle.first(7)
end

# ---------- find_all alias ----------

def test_find_all_alias_for_select
  assert_equal [1, 3], [1, 2, 3, 4].find_all { |x| x.odd? }
end

TESTS = [
  :test_grep_with_range,
  :test_grep_with_class,
  :test_grep_with_block_transforms,
  :test_grep_v,
  :test_grep_on_range,
  :test_find_index_target,
  :test_find_index_block,
  :test_find_index_miss,
  :test_cycle_with_block_n_times,
  :test_cycle_first_n_no_block,
  :test_find_all_alias_for_select,
]
TESTS.each { |t| run_test(t) }
report "GrepCycle"
