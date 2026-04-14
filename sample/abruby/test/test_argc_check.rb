require_relative 'test_helper'

class TestArgcCheck < AbRubyTest
  # ================================================================
  # Fixed arity (required only)
  # ================================================================

  def test_fixed_0_ok         = assert_eval('def f; 42; end; f', 42)
  def test_fixed_1_ok         = assert_eval('def f(a); a; end; f(1)', 1)
  def test_fixed_2_ok         = assert_eval('def f(a,b); a+b; end; f(1,2)', 3)
  def test_fixed_3_ok         = assert_eval('def f(a,b,c); a+b+c; end; f(1,2,3)', 6)

  def test_fixed_0_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f; 1; end; f(99)') }
    assert_match(/given 1, expected 0/, err.message)
  end
  def test_fixed_1_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f(1,2)') }
    assert_match(/given 2, expected 1/, err.message)
  end
  def test_fixed_1_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f') }
    assert_match(/given 0, expected 1/, err.message)
  end
  def test_fixed_2_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a,b); a; end; f(1)') }
    assert_match(/given 1, expected 2/, err.message)
  end
  def test_fixed_2_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a,b); a; end; f(1,2,3)') }
    assert_match(/given 3, expected 2/, err.message)
  end

  # ================================================================
  # Optional parameters (def f(a, b = val))
  # ================================================================

  def test_opt_1_with_default
    assert_eval('def f(a = 10); a; end; f', 10)
  end
  def test_opt_1_with_arg
    assert_eval('def f(a = 10); a; end; f(5)', 5)
  end
  def test_opt_1_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a = 10); a; end; f(1,2)') }
    assert_match(/given 2, expected 0..1/, err.message)
  end

  def test_req_opt_all_provided
    assert_eval('def f(a, b = 20); a + b; end; f(1, 2)', 3)
  end
  def test_req_opt_default_used
    assert_eval('def f(a, b = 20); a + b; end; f(1)', 21)
  end
  def test_req_opt_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 20); a; end; f') }
    assert_match(/given 0, expected 1/, err.message)
  end
  def test_req_opt_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 20); a; end; f(1,2,3)') }
    assert_match(/given 3, expected 1..2/, err.message)
  end

  def test_req_opt2_all_provided
    assert_eval('def f(a, b = 2, c = 3); a + b + c; end; f(1, 20, 30)', 51)
  end
  def test_req_opt2_partial
    assert_eval('def f(a, b = 2, c = 3); a + b + c; end; f(1, 20)', 24)
  end
  def test_req_opt2_minimal
    assert_eval('def f(a, b = 2, c = 3); a + b + c; end; f(1)', 6)
  end

  def test_req_opt2_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b=2, c=3); a; end; f') }
    assert_match(/given 0, expected 1/, err.message)
  end
  def test_req_opt2_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b=2, c=3); a; end; f(1,2,3,4)') }
    assert_match(/given 4, expected 1..3/, err.message)
  end

  def test_opt2_only_none
    assert_eval('def f(a=1, b=2); a+b; end; f', 3)
  end
  def test_opt2_only_one
    assert_eval('def f(a=1, b=2); a+b; end; f(10)', 12)
  end
  def test_opt2_only_both
    assert_eval('def f(a=1, b=2); a+b; end; f(10, 20)', 30)
  end
  def test_opt2_only_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a=1, b=2); a; end; f(1,2,3)') }
    assert_match(/given 3, expected 0..2/, err.message)
  end

  def test_opt_default_expr
    assert_eval('def f(a, b = a * 2); b; end; f(5)', 10)
  end

  def test_opt_nil_not_default
    # explicit nil must NOT trigger the default
    assert_eval('def f(a = 99); a; end; f(nil)', nil)
  end
  def test_opt_false_not_default
    assert_eval('def f(a = 99); a; end; f(false)', false)
  end
  def test_opt_zero_not_default
    assert_eval('def f(a = 99); a; end; f(0)', 0)
  end
  def test_opt2_nil_partial
    # f(nil) → a=nil (explicit), b=2 (default)
    assert_eval('def f(a=1, b=2); [a, b]; end; f(nil)', [nil, 2])
  end
  def test_opt_nil_second
    # f(1, nil) → a=1, b=nil (explicit)
    assert_eval('def f(a, b=99); [a, b]; end; f(1, nil)', [1, nil])
  end

  # ================================================================
  # Rest parameters (def f(*rest))
  # ================================================================

  def test_rest_only_empty
    assert_eval('def f(*r); r.length; end; f', 0)
  end
  def test_rest_only_one
    assert_eval('def f(*r); r.length; end; f(1)', 1)
  end
  def test_rest_only_many
    assert_eval('def f(*r); r.length; end; f(1,2,3,4,5)', 5)
  end
  def test_rest_only_values
    assert_eval('def f(*r); r; end; f(10, 20, 30)', [10, 20, 30])
  end

  def test_req_rest
    assert_eval('def f(a, *r); [a, r]; end; f(1)', [1, []])
  end
  def test_req_rest_with_extras
    assert_eval('def f(a, *r); [a, r]; end; f(1, 2, 3)', [1, [2, 3]])
  end
  def test_req_rest_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, *r); a; end; f') }
    assert_match(/given 0, expected 1\+/, err.message)
  end

  def test_req2_rest
    assert_eval('def f(a, b, *r); [a, b, r.length]; end; f(1, 2)', [1, 2, 0])
  end
  def test_req2_rest_with_extras
    assert_eval('def f(a, b, *r); [a, b, r]; end; f(1, 2, 3, 4)', [1, 2, [3, 4]])
  end
  def test_req2_rest_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b, *r); a; end; f(1)') }
    assert_match(/given 1, expected 2\+/, err.message)
  end

  # ================================================================
  # Optional + Rest combined (def f(a, b = val, *rest))
  # ================================================================

  def test_opt_rest_minimal
    assert_eval('def f(a, b = 10, *r); [a, b, r]; end; f(1)', [1, 10, []])
  end
  def test_opt_rest_with_opt
    assert_eval('def f(a, b = 10, *r); [a, b, r]; end; f(1, 2)', [1, 2, []])
  end
  def test_opt_rest_with_extras
    assert_eval('def f(a, b = 10, *r); [a, b, r]; end; f(1, 2, 3, 4)', [1, 2, [3, 4]])
  end
  def test_opt_rest_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 10, *r); a; end; f') }
    assert_match(/given 0, expected 1\+/, err.message)
  end

  # ================================================================
  # Post parameters (def f(a, *rest, b))
  # ================================================================

  def test_post_pre_rest_post_minimal
    assert_eval('def f(a, *r, b); [a, r, b]; end; f(1, 2)', [1, [], 2])
  end
  def test_post_pre_rest_post_one_in_rest
    assert_eval('def f(a, *r, b); [a, r, b]; end; f(1, 2, 3)', [1, [2], 3])
  end
  def test_post_pre_rest_post_many
    assert_eval('def f(a, *r, b); [a, r, b]; end; f(1, 2, 3, 4, 5)', [1, [2, 3, 4], 5])
  end
  def test_post_pre_rest_post_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, *r, b); a; end; f(1)') }
    assert_match(/given 1, expected 2\+/, err.message)
  end

  def test_post_rest_only_post
    # def f(*rest, b) — no pre, just rest + post
    assert_eval('def f(*r, b); [r, b]; end; f(1)', [[], 1])
  end
  def test_post_rest_only_post_multi
    assert_eval('def f(*r, b); [r, b]; end; f(1, 2, 3)', [[1, 2], 3])
  end
  def test_post_rest_only_post_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(*r, b); b; end; f') }
    assert_match(/given 0, expected 1\+/, err.message)
  end

  def test_post_multi_pre_multi_post
    # def f(a, b, *rest, c, d)
    assert_eval('def f(a, b, *r, c, d); [a, b, r, c, d]; end; f(1, 2, 3, 4)', [1, 2, [], 3, 4])
  end
  def test_post_multi_pre_multi_post_with_rest
    assert_eval('def f(a, b, *r, c, d); [a, b, r, c, d]; end; f(1, 2, 3, 4, 5, 6)',
                [1, 2, [3, 4], 5, 6])
  end
  def test_post_multi_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b, *r, c, d); a; end; f(1, 2, 3)') }
    assert_match(/given 3, expected 4\+/, err.message)
  end

  # ================================================================
  # Post + Optional combinations (def f(a, b = 10, *rest, c))
  # ================================================================

  def test_opt_post_minimal
    # f(1, 2) → a=1, b=default, rest=[], c=2
    assert_eval('def f(a, b = 10, *r, c); [a, b, r, c]; end; f(1, 2)', [1, 10, [], 2])
  end
  def test_opt_post_opt_filled
    # f(1, 2, 3) → a=1, b=2, rest=[], c=3
    assert_eval('def f(a, b = 10, *r, c); [a, b, r, c]; end; f(1, 2, 3)', [1, 2, [], 3])
  end
  def test_opt_post_with_rest
    # f(1, 2, 3, 4) → a=1, b=2, rest=[3], c=4
    assert_eval('def f(a, b = 10, *r, c); [a, b, r, c]; end; f(1, 2, 3, 4)', [1, 2, [3], 4])
  end
  def test_opt_post_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 10, *r, c); a; end; f(1)') }
    assert_match(/given 1, expected 2\+/, err.message)
  end

  def test_opt_post_no_rest
    # def f(a, b = 10, c) — post without rest
    assert_eval('def f(a, b = 10, c); [a, b, c]; end; f(1, 2)', [1, 10, 2])
  end
  def test_opt_post_no_rest_full
    assert_eval('def f(a, b = 10, c); [a, b, c]; end; f(1, 2, 3)', [1, 2, 3])
  end
  def test_opt_post_no_rest_too_few
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 10, c); a; end; f(1)') }
    assert_match(/given 1, expected 2..3/, err.message)
  end
  def test_opt_post_no_rest_too_many
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b = 10, c); a; end; f(1, 2, 3, 4)') }
    assert_match(/given 4, expected 2..3/, err.message)
  end

  def test_post_nil_preserved
    # f(1, nil) with post: nil goes to c, not default
    assert_eval('def f(a, b = 10, c); [a, b, c]; end; f(1, nil)', [1, 10, nil])
  end
  def test_post_nil_with_rest
    # f(1, 2, 3, nil) with post: nil goes to post
    assert_eval('def f(a, *r, b); [a, r, b]; end; f(1, 2, 3, nil)', [1, [2, 3], nil])
  end

  # ================================================================
  # Method call with receiver
  # ================================================================

  def test_method_call_too_many
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; def f; 1; end; end; C.new.f(99)')
    }
    assert_match(/given 1, expected 0/, err.message)
  end
  def test_method_call_too_few
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; def f(a); a; end; end; C.new.f')
    }
    assert_match(/given 0, expected 1/, err.message)
  end
  def test_method_call_opt
    assert_eval(
      'class C; def f(a, b = 5); a + b; end; end; C.new.f(10)', 15)
  end
  def test_method_call_rest
    assert_eval(
      'class C; def f(*r); r.length; end; end; C.new.f(1, 2, 3)', 3)
  end

  # ================================================================
  # attr_reader / attr_writer
  # ================================================================

  def test_attr_reader_too_many
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; attr_reader :x; def initialize; @x = 1; end; end; C.new.x(99)')
    }
    assert_match(/given 1, expected 0/, err.message)
  end
  def test_attr_reader_ok
    assert_eval('class C; attr_reader :x; def initialize; @x = 42; end; end; C.new.x', 42)
  end
  def test_attr_writer_ok
    assert_eval('class C; attr_writer :x; attr_reader :x; end; c = C.new; c.x = 5; c.x', 5)
  end

  # ================================================================
  # rescue catches ArgumentError
  # ================================================================

  def test_rescue_catches
    assert_eval('def f; 1; end; begin; f(1); rescue; "caught"; end', "caught")
  end
  def test_rescue_message
    assert_eval(
      'def f; 1; end; begin; f(1); rescue => e; e.message; end',
      "wrong number of arguments (given 1, expected 0)")
  end
  def test_rescue_opt_message
    assert_eval(
      'def f(a = 1); a; end; begin; f(1, 2); rescue => e; e.message; end',
      "wrong number of arguments (given 2, expected 0..1)")
  end
  def test_rescue_rest_message
    assert_eval(
      'def f(a, *r); a; end; begin; f; rescue => e; e.message; end',
      "wrong number of arguments (given 0, expected 1+)")
  end

  # ================================================================
  # Recursive with wrong args
  # ================================================================

  def test_recursive_wrong_args
    err = assert_raises(RuntimeError) {
      AbRuby.eval('def f(a); if a > 0; f(a - 1, 99); else; 0; end; end; f(3)')
    }
    assert_match(/given 2, expected 1/, err.message)
  end

  # ================================================================
  # Edge cases
  # ================================================================

  def test_rest_anonymous
    # def f(*) — anonymous rest, absorbs extra args
    assert_eval('def f(a, *); a; end; f(1, 2, 3)', 1)
  end

  def test_rest_empty_array
    assert_eval('def f(*r); r; end; f', [])
  end

  def test_rest_single_element
    assert_eval('def f(*r); r; end; f(42)', [42])
  end

  def test_opt_with_method_call_default
    assert_eval('def double(x); x * 2; end; def f(a, b = double(a)); b; end; f(5)', 10)
  end
end
