require_relative "../../test_helper"

def test_if
  assert_equal 1, (1 if true)
  assert_equal nil, (1 if false)
  assert_equal "yes", (true ? "yes" : "no")
  assert_equal "no", (false ? "yes" : "no")
end

def test_if_elsif_else
  def cmp(x, y)
    if x < y
      :lt
    elsif x > y
      :gt
    else
      :eq
    end
  end
  assert_equal :lt, cmp(1, 2)
  assert_equal :gt, cmp(2, 1)
  assert_equal :eq, cmp(1, 1)
end

def test_unless
  assert_equal 1, (1 unless false)
  assert_equal nil, (1 unless true)
end

def test_while
  s = 0
  i = 0
  while i < 5
    s += i
    i += 1
  end
  assert_equal 10, s
end

def test_until
  s = 0
  i = 0
  until i >= 5
    s += i
    i += 1
  end
  assert_equal 10, s
end

def test_do_while
  s = 0
  i = 0
  begin
    s += i
    i += 1
  end while i < 5
  assert_equal 10, s

  # do-while with false condition still runs body once
  ran = 0
  begin
    ran += 1
  end while false
  assert_equal 1, ran
end

def test_case_when
  def categorize(x)
    case x
    when 0 then :zero
    when 1, 2, 3 then :small
    when Integer then :int
    when String then :str
    else :other
    end
  end
  assert_equal :zero, categorize(0)
  assert_equal :small, categorize(2)
  assert_equal :int, categorize(99)
  assert_equal :str, categorize("hi")
  assert_equal :other, categorize(3.14)
end

def test_case_no_subject
  def check(x)
    case
    when x > 0 then :pos
    when x < 0 then :neg
    else :zero
    end
  end
  assert_equal :pos, check(5)
  assert_equal :neg, check(-3)
  assert_equal :zero, check(0)
end

def test_break_in_while
  i = 0
  result = while i < 100
    break i if i == 5
    i += 1
  end
  assert_equal 5, result
end

def test_next_in_while
  arr = []
  i = 0
  while i < 5
    i += 1
    next if i == 3
    arr << i
  end
  assert_equal [1, 2, 4, 5], arr
end

def test_short_circuit
  # && / ||
  x = nil
  y = (x && x.size) || -1
  assert_equal -1, y
  z = "ok" && 42
  assert_equal 42, z
end

def test_and_or_assign
  a = nil
  a ||= 10
  assert_equal 10, a
  a ||= 20
  assert_equal 10, a   # unchanged
  a &&= 99
  assert_equal 99, a
  b = nil
  b &&= 99
  assert_equal nil, b   # short-circuit
end

def test_op_assign
  a = 5
  a += 3
  assert_equal 8, a
  a -= 1
  assert_equal 7, a
  a *= 2
  assert_equal 14, a
  a /= 3
  assert_equal 4, a
  a **= 2
  assert_equal 16, a
end

TESTS = %i[
  test_if test_if_elsif_else test_unless test_while test_until test_do_while
  test_case_when test_case_no_subject
  test_break_in_while test_next_in_while
  test_short_circuit test_and_or_assign test_op_assign
]
TESTS.each {|t| run_test(t) }
report("Control")
