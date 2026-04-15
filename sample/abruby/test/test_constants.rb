require_relative 'test_helper'

class TestConstants < AbRubyTest
  def test_const_assign_and_read
    assert_eval('FOO = 42; FOO', 42)
  end

  def test_const_string
    assert_eval('MSG = "hello"; MSG', "hello")
  end

  def test_const_in_class
    assert_eval('
      class MyClass
        VERSION = 10
      end
      MyClass::VERSION
    ', 10)
  end

  def test_const_toplevel
    assert_eval('PI = 3; PI + 1', 4)
  end

  def test_const_overwrite
    assert_eval('X = 1; X = 2; X', 2)
  end

  def test_const_with_expression
    assert_eval('MAX = 10 * 10; MAX', 100)
  end

  # const_get / const_set
  def test_const_get_symbol
    assert_eval('class TcCG; FOO = 99; end; TcCG.const_get(:FOO)', 99)
  end

  def test_const_get_string
    assert_eval('class TcCG; BAR = 7; end; TcCG.const_get("BAR")', 7)
  end

  def test_const_set_symbol
    assert_eval('class TcCS; end; TcCS.const_set(:X, 42); TcCS::X', 42)
  end

  def test_const_set_string
    assert_eval('class TcCS; end; TcCS.const_set("Y", 10); TcCS::Y', 10)
  end

  # Const lookup from inside a block body must walk the lexical cref chain
  # captured at block-literal creation, not the yielding frame's chain.
  # Regression: cfunc-driven yield (Array#map, Range#map, ...) used to
  # leave current_frame->entry pointing at the cfunc's NULL entry, so the
  # block's const lookup found nothing.
  def test_const_in_cfunc_block_module_class
    assert_eval('
      module TcMod
        TC_X = 12
        class TcInner
          TC_Y = (1..3).map {|i| i * TC_X }
        end
      end
      TcMod::TcInner::TC_Y
    ', [12, 24, 36])
  end

  def test_const_in_ast_yield_block
    assert_eval('
      module TcMod2
        TC_X2 = 7
        class TcInner2
          def run
            yield 5
          end
          TC_Y2 = TcInner2.new.run {|i| i * TC_X2 }
        end
      end
      TcMod2::TcInner2::TC_Y2
    ', 35)
  end
end
