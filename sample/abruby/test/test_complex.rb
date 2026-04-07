require_relative 'test_helper'

class TestComplex < AbRubyTest
  def test_complex_constructor
    assert_eval('Complex(1, 2).to_s', "1+2i")
  end

  def test_imaginary_literal
    assert_eval('2i.to_s', "0+2i")
  end

  def test_complex_arithmetic
    assert_eval('Complex(1, 2) + Complex(3, 4)', Complex(4, 6))
    assert_eval('Complex(5, 3) - Complex(2, 1)', Complex(3, 2))
    assert_eval('Complex(1, 2) * Complex(3, 4)', Complex(-5, 10))
  end

  def test_complex_with_integer
    assert_eval('Complex(1, 2) + 3', Complex(4, 2))
    assert_eval('Complex(2, 3) * 2', Complex(4, 6))
  end

  def test_complex_eq
    assert_eval('Complex(1, 2) == Complex(1, 2)', true)
    assert_eval('Complex(1, 2) == Complex(1, 3)', false)
  end

  def test_complex_real_imaginary
    assert_eval('Complex(3, 4).real', 3)
    assert_eval('Complex(3, 4).imaginary', 4)
  end

  def test_complex_abs
    assert_eval('Complex(3, 4).abs', 5.0)
  end

  def test_complex_conjugate
    assert_eval('Complex(1, 2).conjugate', Complex(1, -2))
  end

  def test_complex_neg
    assert_eval('-Complex(1, 2)', Complex(-1, -2))
  end

  def test_complex_inspect
    assert_eval('Complex(1, 2).inspect', "(1+2i)")
  end

  # TODO: 1 + 2i requires Integer#+ to handle Complex argument
  # which involves nested method calls in argument position
  # def test_complex_from_expression
  #   assert_eval('1 + 2i', Complex(1, 2))
  # end
end
