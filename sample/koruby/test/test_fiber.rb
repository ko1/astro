require_relative "test_helper"

def test_basic
  f = Fiber.new do
    Fiber.yield 1
    Fiber.yield 2
    3
  end
  assert_equal 1, f.resume
  assert_equal 2, f.resume
  assert_equal 3, f.resume
end

def test_resume_with_args
  f = Fiber.new do |a|
    b = Fiber.yield(a + 1)
    c = Fiber.yield(b + 1)
    c + 1
  end
  assert_equal 11, f.resume(10)
  assert_equal 21, f.resume(20)
  assert_equal 31, f.resume(30)
end

def test_state
  f = Fiber.new { :done }
  assert_equal :done, f.resume
  # second resume on dead should raise
  raised = false
  begin
    f.resume
  rescue
    raised = true
  end
  assert_equal true, raised
end

TESTS = %i[
  test_basic test_resume_with_args test_state
]
TESTS.each {|t| run_test(t) }
report("Fiber")
