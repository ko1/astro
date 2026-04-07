require_relative 'test_helper'

class TestRational < AbRubyTest
  def test_rational_constructor
    assert_eval('Rational(1, 3).to_s', "1/3")
  end

  def test_rational_literal
    assert_eval('3r.to_s', "3/1")
  end

  def test_rational_arithmetic
    assert_eval('Rational(1, 3) + Rational(1, 6)', Rational(1, 2))
    assert_eval('Rational(1, 2) - Rational(1, 3)', Rational(1, 6))
    assert_eval('Rational(2, 3) * Rational(3, 4)', Rational(1, 2))
    assert_eval('Rational(1, 2) / Rational(1, 4)', Rational(2, 1))
  end

  def test_rational_comparison
    assert_eval('Rational(1, 3) < Rational(1, 2)', true)
    assert_eval('Rational(1, 2) > Rational(1, 3)', true)
    assert_eval('Rational(1, 2) == Rational(2, 4)', true)
    assert_eval('Rational(1, 2) != Rational(1, 3)', true)
  end

  def test_rational_with_integer
    assert_eval('Rational(1, 2) + 1', Rational(3, 2))
    assert_eval('Rational(3, 2) * 2', Rational(3, 1))
  end

  def test_rational_numerator_denominator
    assert_eval('Rational(3, 6).numerator', 1)
    assert_eval('Rational(3, 6).denominator', 2)
  end

  def test_rational_to_f
    assert_eval('Rational(1, 2).to_f', 0.5)
  end

  def test_rational_to_i
    assert_eval('Rational(7, 3).to_i', 2)
  end

  def test_rational_neg
    assert_eval('-Rational(1, 3)', Rational(-1, 3))
  end

  def test_rational_inspect
    assert_eval('Rational(1, 3).inspect', "(1/3)")
  end
end
