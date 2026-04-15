require_relative 'test_helper'

# Verify that call-kind specialization (node_call0/1/2 → *_ast / *_cfunc /
# *_ivar_*) preserves semantics across monomorphic, polymorphic, and
# method-redefinition scenarios.
class TestCallSpecialize < AbRubyTest
  # call1 on a simple AST method — drives node_call1 → node_call1_ast.
  def test_ast_monomorphic_call1
    assert_eval <<~RUBY, 1000
      def incr(x); x + 1; end
      i = 0
      1000.times { i = incr(i) }
      i
    RUBY
  end

  # attr_reader → ivar_get specialization on node_call0.
  def test_ivar_reader_specialize
    assert_eval <<~RUBY, 42
      class C
        def initialize(v); @v = v; end
        attr_reader :v
      end
      c = C.new(42)
      r = 0
      100.times { r = c.v }
      r
    RUBY
  end

  # attr_writer → ivar_set specialization on node_call1.
  def test_ivar_writer_specialize
    assert_eval <<~RUBY, 100
      class C
        def initialize; @v = 0; end
        attr_accessor :v
      end
      c = C.new
      100.times { |i| c.v = i + 1 }
      c.v
    RUBY
  end

  # Method redefinition bumps method_serial → cache miss in the
  # specialized dispatcher → demote to generic → re-specialize to the
  # new body's mtype.
  def test_method_redefine_invalidates_specialize
    assert_eval <<~RUBY, [20, 110, 110]
      def foo(x); x * 2; end
      r = 0
      100.times { r = foo(10) }
      a = r                 # 20
      def foo(x); x + 100; end
      b = foo(10)           # 110
      100.times { r = foo(10) }
      c = r                 # 110
      [a, b, c]
    RUBY
  end

  # Polymorphic receiver: same call site hits two classes.  First call
  # specializes to one class's mtype; second call sees a klass mismatch,
  # demotes to generic, refills, re-specializes.
  def test_polymorphic_demotes_and_respecializes
    assert_eval <<~RUBY, [11, 12, 11, 12]
      class A; def m(x); x + 1; end; end
      class B; def m(x); x + 2; end; end
      a = A.new
      b = B.new
      def call_m(obj, v); obj.m(v); end
      r1 = call_m(a, 10)    # 11
      r2 = call_m(b, 10)    # 12
      50.times { call_m(a, 10) }
      50.times { call_m(b, 10) }
      r3 = call_m(a, 10)    # 11
      r4 = call_m(b, 10)    # 12
      [r1, r2, r3, r4]
    RUBY
  end

  # cfunc specialization: built-in methods hit prologue_cfunc.
  def test_cfunc_call1
    assert_eval <<~RUBY, [1, 2, 3, 4]
      a = [1, 2, 3]
      100.times { a.push(4); a.pop }
      a.push(4)
      a
    RUBY
  end

  # 2-arg call specialized to node_call2_ast.
  def test_call2_ast
    assert_eval <<~RUBY, 300
      def add(a, b); a + b; end
      r = 0
      100.times { r = add(100, 200) }
      r
    RUBY
  end

  # Megamorphic site: same call hits 3 classes cyclically.  The first
  # few misses specialize+demote; after ABRUBY_CALL_POLY_THRESHOLD the
  # node stays generic and doesn't thrash via swap_dispatcher.  This
  # is the bm_dispatch pattern.  For i in 0..2499, i%3 gives 834 zeros,
  # 833 ones, 833 twos → 834*1 + 833*2 + 833*3 = 4999.
  def test_megamorphic_3class_cycle
    assert_eval <<~RUBY, 4999
      class A; def v; 1; end; end
      class B; def v; 2; end; end
      class C; def v; 3; end; end
      objs = [A.new, B.new, C.new]
      sum = 0
      i = 0
      while i < 2500
        sum += objs[i % 3].v
        i += 1
      end
      sum
    RUBY
  end
end
