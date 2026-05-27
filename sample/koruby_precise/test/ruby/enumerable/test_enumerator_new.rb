require_relative "../../test_helper"

# Enumerator.new { |y| y << v; y.yield v } — Fiber-backed yielder.

def test_basic_yield
  e = Enumerator.new do |y|
    y << 1
    y << 2
    y << 3
  end
  assert_equal 1, e.next
  assert_equal 2, e.next
  assert_equal 3, e.next
  raised = false
  begin
    e.next
  rescue StopIteration
    raised = true
  end
  assert raised
end

def test_yield_method
  e = Enumerator.new do |y|
    y.yield 10
    y.yield 20
  end
  assert_equal 10, e.next
  assert_equal 20, e.next
end

def test_to_a
  e = Enumerator.new do |y|
    5.times { |i| y << i * i }
  end
  assert_equal [0, 1, 4, 9, 16], e.to_a
end

def test_each_block
  e = Enumerator.new do |y|
    y << :a; y << :b; y << :c
  end
  out = []
  e.each { |v| out << v }
  assert_equal [:a, :b, :c], out
end

def test_peek
  e = Enumerator.new do |y|
    y << 1; y << 2
  end
  assert_equal 1, e.peek
  assert_equal 1, e.peek    # idempotent
  assert_equal 1, e.next    # peek doesn't advance past
  assert_equal 2, e.next
end

def test_rewind
  e = Enumerator.new do |y|
    y << 1; y << 2
  end
  e.next; e.next
  e.rewind
  assert_equal 1, e.next
end

def test_first
  e = Enumerator.new do |y|
    y << 1; y << 2; y << 3
  end
  assert_equal 1, e.first
  e.rewind
  assert_equal [1, 2], e.first(2)
end

def test_chain_map
  e = Enumerator.new { |y| 4.times { |i| y << i } }
  doubled = e.map { |x| x * 2 }
  assert_equal [0, 2, 4, 6], doubled.to_a
end

def test_chain_select
  e = Enumerator.new { |y| 6.times { |i| y << i } }
  evens = e.select { |x| x.even? }
  assert_equal [0, 2, 4], evens.to_a
end

def test_infinite_with_first
  fib = Enumerator.new do |y|
    a, b = 0, 1
    loop do
      y << a
      a, b = b, a + b
    end
  end
  assert_equal [0, 1, 1, 2, 3, 5, 8, 13, 21, 34], fib.first(10)
end

def test_no_block_raises
  raised = false
  begin
    Enumerator.new
  rescue ArgumentError
    raised = true
  end
  assert raised
end

TESTS = %i[
  test_basic_yield test_yield_method test_to_a test_each_block
  test_peek test_rewind test_first test_chain_map test_chain_select
  test_infinite_with_first test_no_block_raises
]
TESTS.each {|t| run_test(t) }
report "EnumeratorNew"
