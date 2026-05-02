require_relative "../../test_helper"

# String#gsub!, sub!, scan — added 2026-05-02 (literal-string match
# only; full Regexp is deferred to astrorge).

def test_gsub_bang_replaces
  s = String.new("hello")
  result = s.gsub!("l", "L")
  assert_equal "heLLo", s
  assert_equal "heLLo", result   # gsub! returns self on match
end

def test_gsub_bang_no_match_returns_nil
  s = String.new("hello")
  assert_equal nil, s.gsub!("xx", "yy")
  assert_equal "hello", s        # unchanged on no match
end

def test_sub_bang_replaces_first_only
  s = String.new("hello")
  s.sub!("l", "L")
  assert_equal "heLlo", s
end

def test_sub_bang_no_match_returns_nil
  s = String.new("abc")
  assert_equal nil, s.sub!("xx", "yy")
end

def test_gsub_bang_frozen_raises_or_nils
  s = "hello"
  s.freeze
  raised = false
  begin
    s.gsub!("l", "L")
  rescue
    raised = true
  end
  # Either FrozenError or returns nil — both acceptable.
  assert(raised || s == "hello")
end

# ---------- String#scan ----------

def test_scan_returns_all_matches
  assert_equal ["hello", "hello"], "hello hello".scan("hello")
end

def test_scan_no_match_empty
  assert_equal [], "abc".scan("xyz")
end

def test_scan_overlapping_skipped
  # "aaa".scan("aa") in CRuby returns ["aa"] (non-overlapping).
  assert_equal ["aa"], "aaa".scan("aa")
end

TESTS = [
  :test_gsub_bang_replaces,
  :test_gsub_bang_no_match_returns_nil,
  :test_sub_bang_replaces_first_only,
  :test_sub_bang_no_match_returns_nil,
  :test_gsub_bang_frozen_raises_or_nils,
  :test_scan_returns_all_matches,
  :test_scan_no_match_empty,
  :test_scan_overlapping_skipped,
]
TESTS.each { |t| run_test(t) }
report "StringBang"
