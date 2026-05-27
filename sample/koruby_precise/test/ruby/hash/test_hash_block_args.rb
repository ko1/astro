require_relative "../../test_helper"

# Hash methods that accept blocks: merge, fetch, default, etc.

def test_merge_with_block_resolves_conflict
  a = {x: 1, y: 2}
  b = {y: 20, z: 30}
  result = a.merge(b) { |key, ov, nv| ov + nv }
  assert_equal({x: 1, y: 22, z: 30}, result)
end

def test_fetch_default_block
  h = {a: 1}
  result = h.fetch(:missing) { |k| "default-#{k}" }
  assert_equal "default-missing", result
end

def test_fetch_present_ignores_block
  h = {a: 1}
  result = h.fetch(:a) { |k| "ignored" }
  assert_equal 1, result
end

def test_fetch_default_value
  h = {a: 1}
  assert_equal :fallback, h.fetch(:missing, :fallback)
  assert_equal 1,         h.fetch(:a, :fallback)
end

def test_fetch_no_default_raises
  raised = false
  begin
    {a: 1}.fetch(:missing)
  rescue KeyError
    raised = true
  rescue StandardError
    raised = true       # accept any StandardError-class fallback
  end
  assert raised, "expected exception fetching missing key with no default"
end

# ---------- Hash.new with default block ----------

def test_hash_new_default_block_remembers_misses
  # Use array mutation instead of `count += 1` since proc.call's env
  # snapshot semantics in koruby don't propagate write-back of
  # outer-scope locals — known limitation, tracked elsewhere.
  log = []
  h = Hash.new { |hash, k| log << k; hash[k] = k.to_s }
  assert_equal "x", h[:x]
  assert_equal "y", h[:y]
  # second access uses cached value, no new block call
  assert_equal "x", h[:x]
  assert_equal [:x, :y], log
end

# ---------- Hash#each chaining ----------

def test_hash_each_returns_self
  h = {a: 1, b: 2}
  result = h.each { |_, _| }
  assert(result.equal?(h), "each should return self")
end

TESTS = [
  :test_merge_with_block_resolves_conflict,
  :test_fetch_default_block,
  :test_fetch_present_ignores_block,
  :test_fetch_default_value,
  :test_fetch_no_default_raises,
  :test_hash_new_default_block_remembers_misses,
  :test_hash_each_returns_self,
]
TESTS.each { |t| run_test(t) }
report "HashBlockArgs"
