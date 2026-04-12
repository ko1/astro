require_relative 'test_helper'

class TestOrAndWrite < AbRubyTest
  # Local variables
  def test_lvar_or_write_nil  = assert_eval("a = nil; a ||= 42; a", 42)
  def test_lvar_or_write_false = assert_eval("a = false; a ||= 42; a", 42)
  def test_lvar_or_write_set  = assert_eval("a = 7; a ||= 42; a", 7)
  def test_lvar_and_write_nil = assert_eval("a = nil; a &&= 42; a", nil)
  def test_lvar_and_write_set = assert_eval("a = 7; a &&= 42; a", 42)

  # Instance variables
  def test_ivar_or_write_initial = assert_eval(<<~RUBY, 10)
    class C
      def go; @x ||= 10; @x; end
    end
    C.new.go
  RUBY

  def test_ivar_or_write_existing = assert_eval(<<~RUBY, 5)
    class C
      def go; @x = 5; @x ||= 10; @x; end
    end
    C.new.go
  RUBY

  def test_ivar_and_write = assert_eval(<<~RUBY, 10)
    class C
      def go; @x = 5; @x &&= 10; @x; end
    end
    C.new.go
  RUBY

  # Global variables
  def test_gvar_or_write_initial = assert_eval("$abruby_test_gv = nil; $abruby_test_gv ||= 99; $abruby_test_gv", 99)
  def test_gvar_and_write = assert_eval("$abruby_test_gv2 = 5; $abruby_test_gv2 &&= 11; $abruby_test_gv2", 11)

  # Index target
  def test_index_or_write_nil = assert_eval(<<~RUBY, [1, 2])
    a = [nil, 2]
    a[0] ||= 1
    a
  RUBY

  def test_index_or_write_existing = assert_eval(<<~RUBY, [5, 2])
    a = [5, 2]
    a[0] ||= 1
    a
  RUBY

  def test_hash_index_or_write = assert_eval(<<~RUBY, 42)
    h = {}
    h[:k] ||= 42
    h[:k]
  RUBY

  def test_index_and_write = assert_eval(<<~RUBY, [10, 2])
    a = [5, 2]
    a[0] &&= 10
    a
  RUBY

  def test_index_op_write_plus = assert_eval(<<~RUBY, [13, 2])
    a = [10, 2]
    a[0] += 3
    a
  RUBY

  # Call (attribute) target
  def test_call_or_write_nil = assert_eval(<<~RUBY, 42)
    class C
      attr_accessor :v
    end
    c = C.new
    c.v ||= 42
    c.v
  RUBY

  def test_call_or_write_set = assert_eval(<<~RUBY, 5)
    class C
      attr_accessor :v
    end
    c = C.new
    c.v = 5
    c.v ||= 42
    c.v
  RUBY

  def test_call_and_write = assert_eval(<<~RUBY, 10)
    class C
      attr_accessor :v
    end
    c = C.new
    c.v = 3
    c.v &&= 10
    c.v
  RUBY

  def test_call_op_write_plus = assert_eval(<<~RUBY, 8)
    class C
      attr_accessor :v
    end
    c = C.new
    c.v = 5
    c.v += 3
    c.v
  RUBY

  # Receiver side-effect must execute exactly once
  def test_call_or_write_recv_once = assert_eval(<<~RUBY, 1)
    $count = 0
    class C
      attr_accessor :v
    end
    def get_c
      $count += 1
      @c ||= C.new
      @c
    end
    get_c.v ||= 42
    $count
  RUBY

  # Chained nested index ||= (ppu.rb-ish pattern)
  def test_nested_index_or_write = assert_eval(<<~RUBY, [[0, [7]]])
    a = [nil]
    ((a[0] ||= [0, nil])[1] ||= []) << 7
    a
  RUBY

  # Constant or-write
  def test_const_or_write = assert_eval(<<~RUBY, 99)
    CONST_OR_TEST = nil
    CONST_OR_TEST ||= 99
    CONST_OR_TEST
  RUBY
end
