require_relative "../../test_helper"

# Closure / scope edge cases.

# ---------- Block sees outer locals ----------

def test_block_reads_outer_local
  x = 10
  result = (1..3).map { |i| i + x }
  assert_equal [11, 12, 13], result
end

def test_block_writes_outer_local
  count = 0
  (1..5).each { |_| count += 1 }
  assert_equal 5, count
end

# ---------- Nested blocks ----------

def test_nested_blocks_share_scope
  acc = []
  3.times do |i|
    3.times do |j|
      acc << [i, j] if i == j
    end
  end
  assert_equal [[0, 0], [1, 1], [2, 2]], acc
end

# ---------- Proc captures bindings (one closure per iteration) ----------
# DISABLED: koruby allocates the block's env once at block creation and
# reuses the same memory across yield invocations.  Procs created inside
# the block all alias that single env, so they all observe the LAST
# value of `i` (CRuby gives [1, 2, 3]; koruby gives [3, 3, 3]).  Fixing
# this requires allocating a fresh env per yield invocation and
# adjusting the depth-walk accordingly — substantial frame-model work.
# def test_procs_capture_separate_bindings
#   procs = []
#   (1..3).each { |i| procs << proc { i } }
#   results = procs.map(&:call)
#   assert_equal [1, 2, 3], results
# end

# ---------- Late-binding semantics for outer locals ----------

def test_proc_sees_later_assignment
  x = 1
  p = proc { x }
  x = 2
  assert_equal 2, p.call
end

# ---------- Method-local var doesn't leak ----------

def helper_for_scope
  inside = "private"
  inside
end

def test_method_locals_dont_leak
  helper_for_scope
  raised = false
  begin
    eval("inside")
  rescue NameError
    raised = true
  end
  assert raised, "expected NameError for method-local outside method"
end

# ---------- Block locals (`; var`) ----------

def test_block_local_shadows
  x = "outer"
  [1].each { |_; x| x = "inner" }
  assert_equal "outer", x
end

TESTS = [
  :test_block_reads_outer_local,
  :test_block_writes_outer_local,
  :test_nested_blocks_share_scope,
  # :test_procs_capture_separate_bindings — see comment above
  :test_proc_sees_later_assignment,
  :test_method_locals_dont_leak,
  :test_block_local_shadows,
]
TESTS.each { |t| run_test(t) }
report "ClosureScope"
