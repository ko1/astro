require_relative 'test_helper'

class TestGlobalVariables < AbRubyTest
  def test_gvar_write_and_read
    assert_eval('$x = 42; $x', 42)
  end

  def test_gvar_default_nil
    assert_eval('$undefined', nil)
  end

  def test_gvar_string
    assert_eval('$name = "hello"; $name', "hello")
  end

  def test_gvar_compound_assignment
    assert_eval('$x = 10; $x += 5; $x', 15)
  end

  def test_gvar_multiple
    assert_eval('$a = 1; $b = 2; $a + $b', 3)
  end

  def test_gvar_overwrite
    assert_eval('$x = 1; $x = 2; $x', 2)
  end

  def test_gvar_in_method
    assert_eval('
      $g = 0
      def inc
        $g += 1
      end
      inc
      inc
      inc
      $g
    ', 3)
  end

  def test_gvar_shared_across_methods
    assert_eval('
      def set_it
        $shared = 99
      end
      def get_it
        $shared
      end
      set_it
      get_it
    ', 99)
  end

  def test_gvar_in_class
    assert_eval('
      $counter = 0
      class Foo
        def initialize
          $counter += 1
        end
      end
      Foo.new
      Foo.new
      $counter
    ', 2)
  end
end
