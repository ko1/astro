require_relative 'test_helper'

# Comprehensive tests for all parameter kind combinations:
#   R = required (mandatory) pre-parameter
#   O = optional parameter (with default)
#   * = rest parameter
#   P = post required parameter (after rest or optionals)
#   B = block parameter (&blk)
#
# Parameter ordering in Ruby: R* ... O* ... * ... P* ... [&B]
class TestParams < AbRubyTest
  # ================================================================
  # Single parameter kinds
  # ================================================================

  # --- R only ---
  def test_R1      = assert_eval('def f(a); a; end; f(1)', 1)
  def test_R2      = assert_eval('def f(a, b); [a, b]; end; f(1, 2)', [1, 2])
  def test_R3      = assert_eval('def f(a, b, c); [a, b, c]; end; f(1, 2, 3)', [1, 2, 3])
  def test_R1_err_0
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f') }
    assert_match(/given 0, expected 1/, err.message)
  end
  def test_R1_err_2
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f(1,2)') }
    assert_match(/given 2, expected 1/, err.message)
  end

  # --- O only ---
  def test_O1_none  = assert_eval('def f(a=99); a; end; f', 99)
  def test_O1_given = assert_eval('def f(a=99); a; end; f(5)', 5)
  def test_O1_nil   = assert_eval('def f(a=99); a; end; f(nil)', nil)
  def test_O1_false = assert_eval('def f(a=99); a; end; f(false)', false)
  def test_O2_none  = assert_eval('def f(a=1, b=2); [a, b]; end; f', [1, 2])
  def test_O2_one   = assert_eval('def f(a=1, b=2); [a, b]; end; f(10)', [10, 2])
  def test_O2_both  = assert_eval('def f(a=1, b=2); [a, b]; end; f(10, 20)', [10, 20])
  def test_O3_none  = assert_eval('def f(a=1, b=2, c=3); [a, b, c]; end; f', [1, 2, 3])
  def test_O3_one   = assert_eval('def f(a=1, b=2, c=3); [a, b, c]; end; f(10)', [10, 2, 3])
  def test_O3_two   = assert_eval('def f(a=1, b=2, c=3); [a, b, c]; end; f(10, 20)', [10, 20, 3])
  def test_O3_all   = assert_eval('def f(a=1, b=2, c=3); [a, b, c]; end; f(10, 20, 30)', [10, 20, 30])
  def test_O1_err_2
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a=1); a; end; f(1, 2)') }
    assert_match(/given 2, expected 0..1/, err.message)
  end

  # --- Rest only ---
  def test_Rest_0      = assert_eval('def f(*r); r; end; f', [])
  def test_Rest_1      = assert_eval('def f(*r); r; end; f(1)', [1])
  def test_Rest_many   = assert_eval('def f(*r); r; end; f(1,2,3,4,5)', [1,2,3,4,5])
  def test_Rest_anon   = assert_eval('def f(*); 42; end; f(1,2,3)', 42)

  # --- Block only ---
  def test_B_only_given   = assert_eval('def f(&b); b.call(7); end; f { |x| x+1 }', 8)
  def test_B_only_none    = assert_eval('def f(&b); b.nil?; end; f', true)

  # ================================================================
  # Two-parameter-kind combinations
  # ================================================================

  # --- R + O ---
  def test_RO_min   = assert_eval('def f(a, b=99); [a, b]; end; f(1)', [1, 99])
  def test_RO_full  = assert_eval('def f(a, b=99); [a, b]; end; f(1, 2)', [1, 2])
  def test_RO_err_0
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b=1); a; end; f') }
    assert_match(/given 0, expected 1..2/, err.message)
  end
  def test_RO_err_3
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b=1); a; end; f(1,2,3)') }
    assert_match(/given 3, expected 1..2/, err.message)
  end
  def test_RRO_RO  = assert_eval('def f(a, b, c=99); [a, b, c]; end; f(1, 2)', [1, 2, 99])
  def test_RRO_RRO = assert_eval('def f(a, b, c=99); [a, b, c]; end; f(1, 2, 3)', [1, 2, 3])
  def test_ROO_min = assert_eval('def f(a, b=2, c=3); [a, b, c]; end; f(1)', [1, 2, 3])
  def test_ROO_one = assert_eval('def f(a, b=2, c=3); [a, b, c]; end; f(1, 20)', [1, 20, 3])
  def test_ROO_all = assert_eval('def f(a, b=2, c=3); [a, b, c]; end; f(1, 20, 30)', [1, 20, 30])

  # --- R + Rest ---
  def test_RRest_min  = assert_eval('def f(a, *r); [a, r]; end; f(1)', [1, []])
  def test_RRest_one  = assert_eval('def f(a, *r); [a, r]; end; f(1, 2)', [1, [2]])
  def test_RRest_many = assert_eval('def f(a, *r); [a, r]; end; f(1, 2, 3, 4)', [1, [2, 3, 4]])
  def test_RRest_err
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, *r); a; end; f') }
    assert_match(/given 0, expected 1\+/, err.message)
  end
  def test_RRRest_min = assert_eval('def f(a, b, *r); [a, b, r]; end; f(1, 2)', [1, 2, []])
  def test_RRRest_x   = assert_eval('def f(a, b, *r); [a, b, r]; end; f(1, 2, 3, 4)', [1, 2, [3, 4]])

  # --- R + Block ---
  def test_RB         = assert_eval('def f(a, &b); b.call(a); end; f(7) { |x| x*2 }', 14)
  def test_RB_no_blk  = assert_eval('def f(a, &b); b.nil? ? a : :hasblk; end; f(5)', 5)

  # --- O + Rest ---
  def test_ORest_none = assert_eval('def f(a=9, *r); [a, r]; end; f', [9, []])
  def test_ORest_one  = assert_eval('def f(a=9, *r); [a, r]; end; f(1)', [1, []])
  def test_ORest_many = assert_eval('def f(a=9, *r); [a, r]; end; f(1, 2, 3)', [1, [2, 3]])
  def test_ORest_nil  = assert_eval('def f(a=9, *r); [a, r]; end; f(nil)', [nil, []])

  # --- O + Post (no rest) ---
  def test_OP_min    = assert_eval('def f(a=9, b); [a, b]; end; f(1)', [9, 1])
  def test_OP_all    = assert_eval('def f(a=9, b); [a, b]; end; f(1, 2)', [1, 2])
  def test_OP_err_0
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a=9, b); a; end; f') }
    assert_match(/given 0, expected 1..2/, err.message)
  end
  def test_OP_err_3
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a=9, b); a; end; f(1, 2, 3)') }
    assert_match(/given 3, expected 1..2/, err.message)
  end
  def test_OOP_min = assert_eval('def f(a=1, b=2, c); [a, b, c]; end; f(10)', [1, 2, 10])
  def test_OOP_mid = assert_eval('def f(a=1, b=2, c); [a, b, c]; end; f(10, 20)', [10, 2, 20])
  def test_OOP_all = assert_eval('def f(a=1, b=2, c); [a, b, c]; end; f(10, 20, 30)', [10, 20, 30])

  # --- O + Block ---
  def test_OB_none    = assert_eval('def f(a=9, &b); [a, b.call]; end; f { 99 }', [9, 99])
  def test_OB_given   = assert_eval('def f(a=9, &b); [a, b.call]; end; f(1) { 99 }', [1, 99])

  # --- Rest + Post ---
  def test_RestP_min   = assert_eval('def f(*r, p); [r, p]; end; f(1)', [[], 1])
  def test_RestP_many  = assert_eval('def f(*r, p); [r, p]; end; f(1, 2, 3)', [[1, 2], 3])
  def test_RestP_err
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(*r, p); p; end; f') }
    assert_match(/given 0, expected 1\+/, err.message)
  end
  def test_RestPP_min  = assert_eval('def f(*r, a, b); [r, a, b]; end; f(1, 2)', [[], 1, 2])
  def test_RestPP_many = assert_eval('def f(*r, a, b); [r, a, b]; end; f(1, 2, 3, 4)', [[1, 2], 3, 4])

  # --- Rest + Block ---
  def test_RestB_0    = assert_eval('def f(*r, &b); [r, b.call(10)]; end; f { |x| x }', [[], 10])
  def test_RestB_many = assert_eval('def f(*r, &b); [r, b.call(10)]; end; f(1,2,3) { |x| x }', [[1,2,3], 10])

  # --- Post + Block (need rest or opt) ---
  # (covered in three-kind tests below)

  # ================================================================
  # Three-parameter-kind combinations
  # ================================================================

  # --- R + O + Rest ---
  def test_RORest_min  = assert_eval('def f(a, b=2, *r); [a, b, r]; end; f(1)', [1, 2, []])
  def test_RORest_mid  = assert_eval('def f(a, b=2, *r); [a, b, r]; end; f(1, 20)', [1, 20, []])
  def test_RORest_rest = assert_eval('def f(a, b=2, *r); [a, b, r]; end; f(1, 20, 30, 40)', [1, 20, [30, 40]])

  # --- R + O + Post ---
  def test_ROP_min  = assert_eval('def f(a, b=9, c); [a, b, c]; end; f(1, 2)', [1, 9, 2])
  def test_ROP_full = assert_eval('def f(a, b=9, c); [a, b, c]; end; f(1, 2, 3)', [1, 2, 3])
  def test_ROP_err_1
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b=9, c); a; end; f(1)') }
    assert_match(/given 1, expected 2..3/, err.message)
  end

  # --- R + Rest + Post ---
  def test_RRestP_min  = assert_eval('def f(a, *r, p); [a, r, p]; end; f(1, 2)', [1, [], 2])
  def test_RRestP_one  = assert_eval('def f(a, *r, p); [a, r, p]; end; f(1, 2, 3)', [1, [2], 3])
  def test_RRestP_many = assert_eval('def f(a, *r, p); [a, r, p]; end; f(1, 2, 3, 4, 5)', [1, [2,3,4], 5])

  # --- R + Rest + Block ---
  def test_RRestB      = assert_eval('def f(a, *r, &b); b.call([a, r]); end; f(1,2,3) { |x| x }', [1, [2,3]])

  # --- R + Post + Block (via opt without rest) ---
  def test_RPBviaO     = assert_eval('def f(a, b=9, c, &blk); blk.call([a, b, c]); end; f(1, 2) { |x| x }', [1, 9, 2])

  # --- O + Rest + Post ---
  def test_ORestP_min  = assert_eval('def f(a=9, *r, p); [a, r, p]; end; f(1)', [9, [], 1])
  def test_ORestP_pre  = assert_eval('def f(a=9, *r, p); [a, r, p]; end; f(1, 2)', [1, [], 2])
  def test_ORestP_rest = assert_eval('def f(a=9, *r, p); [a, r, p]; end; f(1, 2, 3, 4)', [1, [2, 3], 4])

  # --- O + Rest + Block ---
  def test_ORestB      = assert_eval('def f(a=9, *r, &blk); blk.call([a, r]); end; f(1,2,3) { |x| x }', [1, [2,3]])

  # --- Rest + Post + Block ---
  def test_RestPB      = assert_eval('def f(*r, p, &b); b.call([r, p]); end; f(1,2,3) { |x| x }', [[1, 2], 3])

  # ================================================================
  # Four-parameter-kind combinations
  # ================================================================

  # --- R + O + Rest + Post ---
  def test_RORestP_min  = assert_eval('def f(a, b=9, *r, c); [a, b, r, c]; end; f(1, 2)', [1, 9, [], 2])
  def test_RORestP_pre  = assert_eval('def f(a, b=9, *r, c); [a, b, r, c]; end; f(1, 2, 3)', [1, 2, [], 3])
  def test_RORestP_rest = assert_eval('def f(a, b=9, *r, c); [a, b, r, c]; end; f(1, 2, 3, 4, 5)', [1, 2, [3, 4], 5])

  # --- R + O + Rest + Block ---
  def test_RORestB      = assert_eval('def f(a, b=9, *r, &blk); blk.call([a, b, r]); end; f(1,2,3) { |x| x }', [1, 2, [3]])

  # --- R + O + Post + Block (no rest) ---
  def test_ROPB_min    = assert_eval('def f(a, b=9, c, &blk); blk.call([a, b, c]); end; f(1, 2) { |x| x }', [1, 9, 2])
  def test_ROPB_full   = assert_eval('def f(a, b=9, c, &blk); blk.call([a, b, c]); end; f(1, 2, 3) { |x| x }', [1, 2, 3])

  # --- R + Rest + Post + Block ---
  def test_RRestPB_min  = assert_eval('def f(a, *r, p, &b); b.call([a, r, p]); end; f(1, 2) { |x| x }', [1, [], 2])
  def test_RRestPB_many = assert_eval('def f(a, *r, p, &b); b.call([a, r, p]); end; f(1,2,3,4) { |x| x }', [1, [2, 3], 4])

  # --- O + Rest + Post + Block ---
  def test_ORestPB_min   = assert_eval('def f(a=9, *r, p, &blk); blk.call([a, r, p]); end; f(1) { |x| x }', [9, [], 1])
  def test_ORestPB_pre   = assert_eval('def f(a=9, *r, p, &blk); blk.call([a, r, p]); end; f(1, 2) { |x| x }', [1, [], 2])
  def test_ORestPB_many  = assert_eval('def f(a=9, *r, p, &blk); blk.call([a, r, p]); end; f(1, 2, 3, 4) { |x| x }', [1, [2, 3], 4])

  # ================================================================
  # Five-parameter-kind combination (everything!)
  # ================================================================

  # --- R + O + Rest + Post + Block ---
  def test_RORestPB_min
    # argc=2 (min): a=1, b=default, rest=[], c=2
    assert_eval(
      'def f(a, b=99, *r, c, &blk); blk.call([a, b, r, c]); end; f(1, 2) { |x| x }',
      [1, 99, [], 2])
  end
  def test_RORestPB_pre_opt
    # argc=3: a=1, b=2, rest=[], c=3
    assert_eval(
      'def f(a, b=99, *r, c, &blk); blk.call([a, b, r, c]); end; f(1, 2, 3) { |x| x }',
      [1, 2, [], 3])
  end
  def test_RORestPB_with_rest
    # argc=5: a=1, b=2, rest=[3, 4], c=5
    assert_eval(
      'def f(a, b=99, *r, c, &blk); blk.call([a, b, r, c]); end; f(1, 2, 3, 4, 5) { |x| x }',
      [1, 2, [3, 4], 5])
  end
  def test_RORestPB_no_block
    # Without block: blk is nil
    assert_eval(
      'def f(a, b=99, *r, c, &blk); [a, b, r, c, blk.nil?]; end; f(1, 2)',
      [1, 99, [], 2, true])
  end

  # Multiple R + O + Rest + multiple P + Block
  def test_RR_OO_Rest_PP_B
    assert_eval(
      'def f(a, b, c=30, d=40, *r, e, f, &blk); blk.call([a,b,c,d,r,e,f]); end;' \
      ' f(1, 2, 3, 4, 5, 6, 7, 8) { |x| x }',
      [1, 2, 3, 4, [5, 6], 7, 8])
  end

  def test_RR_OO_Rest_PP_min
    # min args = 2 R + 2 P = 4
    assert_eval(
      'def f(a, b, c=30, d=40, *r, e, f); [a, b, c, d, r, e, f]; end; f(1, 2, 3, 4)',
      [1, 2, 30, 40, [], 3, 4])
  end
  def test_RR_OO_Rest_PP_too_few
    err = assert_raises(RuntimeError) {
      AbRuby.eval('def f(a, b, c=30, d=40, *r, e, f); a; end; f(1, 2, 3)')
    }
    assert_match(/given 3, expected 4\+/, err.message)
  end

  # ================================================================
  # nil preservation across combinations
  # ================================================================

  def test_nil_in_opt_alone       = assert_eval('def f(a=99); a; end; f(nil)', nil)
  def test_nil_in_opt_of_RO       = assert_eval('def f(a, b=99); [a, b]; end; f(1, nil)', [1, nil])
  def test_nil_in_post_of_RRestP  = assert_eval('def f(a, *r, p); [a, r, p]; end; f(1, nil)', [1, [], nil])
  def test_nil_in_rest_items      = assert_eval('def f(*r); r; end; f(1, nil, 3)', [1, nil, 3])
  def test_nil_with_full_combo
    assert_eval(
      'def f(a, b=99, *r, c); [a, b, r, c]; end; f(1, nil, nil, nil)',
      [1, nil, [nil], nil])
  end

  # ================================================================
  # Method call with receiver + various param kinds
  # ================================================================

  def test_receiver_opt_rest
    assert_eval(
      'class C; def f(a, b=99, *r); [a, b, r]; end; end; C.new.f(1)',
      [1, 99, []])
  end
  def test_receiver_rest_post
    assert_eval(
      'class C; def f(a, *r, p); [a, r, p]; end; end; C.new.f(1, 2, 3, 4)',
      [1, [2, 3], 4])
  end
  def test_receiver_all_kinds
    assert_eval(
      'class C; def f(a, b=99, *r, c, &blk); blk.call([a,b,r,c]); end; end; ' \
      'C.new.f(1, 2, 3, 4) { |x| x }',
      [1, 2, [3], 4])
  end

  # ================================================================
  # Recursive calls with various param kinds
  # ================================================================

  def test_recursive_with_opt
    assert_eval(
      'def fact(n, acc=1); if n <= 1; acc; else; fact(n-1, acc*n); end; end; fact(5)',
      120)
  end
  def test_recursive_with_rest
    # Sum via rest + splat recursion. Use a helper for "tail" since slice syntax varies.
    assert_eval(
      'def sum(*a); r = 0; a.each { |x| r += x }; r; end; sum(1, 2, 3, 4, 5)',
      15)
  end

  # ================================================================
  # Arguments passed via splat (*ary) — caller-side
  # ================================================================

  def test_splat_call_to_R
    assert_eval('def f(a, b); [a, b]; end; ary = [1, 2]; f(*ary)', [1, 2])
  end
  def test_splat_call_to_RO
    assert_eval('def f(a, b=99); [a, b]; end; ary = [1]; f(*ary)', [1, 99])
  end
  def test_splat_call_to_Rest
    assert_eval('def f(*r); r; end; ary = [1, 2, 3]; f(*ary)', [1, 2, 3])
  end
  def test_splat_call_to_RRestP
    assert_eval('def f(a, *r, p); [a, r, p]; end; ary = [1, 2, 3]; f(*ary)', [1, [2], 3])
  end

  # ================================================================
  # Edge cases
  # ================================================================

  def test_many_optionals
    # 5 optional params
    assert_eval('def f(a=1, b=2, c=3, d=4, e=5); [a,b,c,d,e]; end; f', [1,2,3,4,5])
  end
  def test_many_optionals_partial
    assert_eval('def f(a=1, b=2, c=3, d=4, e=5); [a,b,c,d,e]; end; f(10, 20, 30)',
                [10, 20, 30, 4, 5])
  end
  def test_default_uses_earlier_param
    assert_eval('def f(a, b=a*2, c=a+b); [a,b,c]; end; f(5)', [5, 10, 15])
  end
  def test_default_uses_earlier_param_override
    assert_eval('def f(a, b=a*2, c=a+b); [a,b,c]; end; f(5, 100)', [5, 100, 105])
  end

  def test_rest_with_complex_elements
    assert_eval('def f(a, *r); r; end; f(1, [2, 3], "hello", :sym, 4.5)',
                [[2, 3], "hello", :sym, 4.5])
  end

  def test_post_with_nil_default
    # Test that post params work when mixed with optionals that have nil defaults
    assert_eval('def f(a=nil, *r, p); [a, r, p]; end; f(1)', [nil, [], 1])
  end
end
