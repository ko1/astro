require_relative "../../test_helper"

# These tests exercise koruby's inline `[]` fast paths (Array index,
# Hash key, Integer bit, String byte).  They duplicate some coverage
# from the per-class tests on purpose: the goal here is to keep the
# inline paths in `node_aref` (and any future split kinds) honest about
# semantics, edge cases, and the basic-op-redef bypass.

# ---- Integer#[] (bit access) ----

def test_int_bit_low_high
  assert_equal 1, 0b1010[1]
  assert_equal 0, 0b1010[0]
  assert_equal 1, 0b1010[3]
  assert_equal 0, 0xFF[8]      # bit beyond set range
end

def test_int_bit_oob_returns_zero
  assert_equal 0, 1[100]       # way out of range
  assert_equal 0, 1[-1]        # negative index → 0
  assert_equal 0, 0xFF[63]     # at boundary
end

def test_int_bit_zero_self
  assert_equal 0, 0[0]
  assert_equal 0, 0[5]
end

def test_int_bit_inside_loop
  # Hot pattern from optcarrot's PPU: bit-extract on Fixnum across
  # iterations; ensure inline path stays correct under repeated calls.
  byte = 0b11010110
  bits = (0..7).map { |b| byte[b] }
  assert_equal [0, 1, 1, 0, 1, 0, 1, 1], bits
end

# ---- Array#[] ----

def test_array_aref_basic
  a = [10, 20, 30]
  assert_equal 10, a[0]
  assert_equal 30, a[2]
  assert_equal nil, a[5]
end

def test_array_aref_negative
  a = [10, 20, 30]
  assert_equal 30, a[-1]
  assert_equal 20, a[-2]
end

# ---- Hash#[] ----

def test_hash_aref_basic
  h = {a: 1, b: 2}
  assert_equal 1, h[:a]
  assert_equal 2, h[:b]
  assert_equal nil, h[:missing]
end

def test_hash_aref_default_value
  h = Hash.new(:fallback)
  h[:set] = 99
  assert_equal 99, h[:set]
  assert_equal :fallback, h[:missing]
end

def test_hash_aref_default_proc_invoked
  # Critical: the inline Hash fast path must NOT short-circuit when
  # default_proc is set — that's the bug that masked optcarrot's
  # rendering loop and hurt earlier.
  calls = []
  h = Hash.new { |hash, key| calls << key; "default-#{key}" }
  h[:preset] = "set"
  assert_equal "set", h[:preset]
  assert_equal "default-x", h[:x]
  assert_equal "default-y", h[:y]
  assert_equal [:x, :y], calls
end

# ---- String#[] ----

def test_string_aref_byte
  s = "hello"
  assert_equal "h", s[0]
  assert_equal "o", s[4]
  assert_equal "o", s[-1]
  assert_equal nil, s[10]
end

# ---- Polymorphic call site ----

def call_aref(x, k)
  # Same expression hit by Array, Hash, String, Integer in succession;
  # makes sure no inline path corrupts state for the next type.
  x[k]
end

def test_polymorphic_aref_site
  assert_equal 20, call_aref([10, 20, 30], 1)
  assert_equal 1,  call_aref({a: 1}, :a)
  assert_equal "h", call_aref("hello", 0)
  assert_equal 1,  call_aref(0b10, 1)
  # Repeat to make sure the site stays correct after multiple types.
  assert_equal 30, call_aref([10, 20, 30], 2)
  assert_equal "e", call_aref("hello", 1)
end

# ---- Redef invalidation ----

# These have to be in their own scope; defining `Array#[]` globally
# would pollute every other test in the suite.  Run as a sub-process
# so the redef isolated.

def run_subproc(src)
  IO.popen([RbConfig.ruby || "./koruby", "-e", src].compact, &:read) rescue nil
end

# Instead of subprocess, we test the runtime guard by introducing a
# very specific class+method combo whose effect is visible.  The
# redef-flag guard already disables the inline path globally; once
# we set it, the cfunc dispatch picks up the redefined method.
class IntegerAcc
  def [](b)
    "indexed:#{b}"
  end
end

def test_custom_class_aref_dispatches_to_user_method
  acc = IntegerAcc.new
  assert_equal "indexed:7", acc[7]
end

# Run all the inline-path tests FIRST (before any basic-op redef has
# flipped the global guard), then do the redef tests.  Re-opening
# Array at top-level happens at load time, before any `run_test` call,
# so the redef has to wait until after we drive run_test() for the
# inline-path tests.
INLINE_TESTS = [
  :test_int_bit_low_high,
  :test_int_bit_oob_returns_zero,
  :test_int_bit_zero_self,
  :test_int_bit_inside_loop,
  :test_array_aref_basic,
  :test_array_aref_negative,
  :test_hash_aref_basic,
  :test_hash_aref_default_value,
  :test_hash_aref_default_proc_invoked,
  :test_string_aref_byte,
  :test_polymorphic_aref_site,
  :test_custom_class_aref_dispatches_to_user_method,
]
INLINE_TESTS.each { |t| run_test(t) }

# After Array#[] is redefined, the inline fast path must back off.
class Array
  alias __orig_brackets []
  def [](i)
    "aref:#{i}"
  end
end

def test_array_aref_redef_takes_effect
  a = [10, 20, 30]
  assert_equal "aref:0", a[0]
  assert_equal "aref:1", a[1]
end

run_test :test_array_aref_redef_takes_effect

# Restore Array#[] semantics — the global redef-flag stays flipped
# (it's monotonic) but at least functional correctness comes back via
# the slow path.
class Array
  alias [] __orig_brackets
end

def test_array_aref_after_restore
  a = [10, 20, 30]
  assert_equal 10, a[0]
  assert_equal 30, a[2]
end

run_test :test_array_aref_after_restore
report "ArefInline"
