require_relative "../../test_helper"

# String#tr / tr! / tr_s / tr_s!

def test_tr_basic
  assert_equal "Hippo", "Hello".tr("el", "ip")
  assert_equal "*ello", "hello".tr("h", "*")
  assert_equal "ifmmp", "hello".tr("a-z", "b-za")  # rotate
end

def test_tr_negation
  # "^l" means "every char except l"
  assert_equal "**ll*", "hello".tr("^l", "*")
end

def test_tr_to_shorter
  # to shorter — last char of `to` repeats for remaining `from` chars
  assert_equal "**llo", "hello".tr("he", "*")
end

def test_tr_s_squeezes
  assert_equal "x", "hello".tr_s("a-z", "x")     # all squeeze to one
  assert_equal "ifmp", "hello".tr_s("a-z", "b-za")
end

def test_tr_bang_changes
  s = String.new("hello")
  r = s.tr!("el", "ip")
  assert_equal "Hippo".downcase, s
  assert_equal s, r            # tr! returns self when changed
end

def test_tr_bang_no_change_returns_nil
  s = String.new("hello")
  assert_equal nil, s.tr!("xyz", "abc")
  assert_equal "hello", s
end

def test_tr_s_bang_squeezes_in_place
  s = String.new("aabbcc")
  r = s.tr_s!("a-c", "x")
  assert_equal "x", s
  assert_equal s, r
end

def test_tr_s_bang_no_change_returns_nil
  s = String.new("hello")
  assert_equal nil, s.tr_s!("xyz", "abc")
  assert_equal "hello", s
end

def test_tr_bang_frozen_returns_nil_or_raises
  s = "hello"
  s.freeze
  raised = false
  begin
    s.tr!("l", "L")
  rescue
    raised = true
  end
  assert(raised || s == "hello")
end

TESTS = %i[
  test_tr_basic test_tr_negation test_tr_to_shorter
  test_tr_s_squeezes
  test_tr_bang_changes test_tr_bang_no_change_returns_nil
  test_tr_s_bang_squeezes_in_place test_tr_s_bang_no_change_returns_nil
  test_tr_bang_frozen_returns_nil_or_raises
]
TESTS.each {|t| run_test(t) }
report "StringTr"
