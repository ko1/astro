require_relative "../../test_helper"

# Kernel methods in non-trivial contexts.

# ---------- nil? ----------

def test_nil_p
  assert_equal true,  nil.nil?
  assert_equal false, false.nil?
  assert_equal false, 0.nil?
  assert_equal false, "".nil?
end

# ---------- Integer / Float / String conversion ----------

def test_integer_kernel
  assert_equal 42, Integer(42)
  assert_equal 42, Integer("42")
  assert_equal 255, Integer("0xff")
  assert_equal 8,   Integer("0o10")
end

def test_integer_kernel_bad_input
  raised = false
  begin
    Integer("abc")
  rescue ArgumentError
    raised = true
  end
  assert raised, "expected ArgumentError on Integer('abc')"
end

def test_float_kernel
  assert_equal 1.5,   Float(1.5)
  assert_equal 1.5,   Float("1.5")
  assert_equal 1e3,   Float("1e3")
end

def test_string_kernel
  assert_equal "42",       String(42)
  assert_equal "1.5",      String(1.5)
  assert_equal "[1, 2]",   String([1, 2])
  assert_equal "abc",      String("abc")
end

# ---------- to_s on various ----------

def test_to_s_nil
  assert_equal "", nil.to_s
end

def test_to_s_true_false
  assert_equal "true",  true.to_s
  assert_equal "false", false.to_s
end

# ---------- !! double bang ----------

def test_double_bang
  assert_equal true,  !!1
  assert_equal true,  !!"x"
  assert_equal true,  !![]
  assert_equal false, !!nil
  assert_equal false, !!false
end

# ---------- gets / puts placeholder ----------

# (puts is hard to test capturing stdout, skip)

TESTS = [
  :test_nil_p,
  :test_integer_kernel,
  :test_integer_kernel_bad_input,
  :test_float_kernel,
  :test_string_kernel,
  :test_to_s_nil,
  :test_to_s_true_false,
  :test_double_bang,
]
TESTS.each { |t| run_test(t) }
report "KernelMethods"
