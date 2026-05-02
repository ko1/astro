require_relative "../../test_helper"

# Exception globals: $! (current exception), bare `raise` re-raises $!,
# multiple chained rescue accessing $!.

def test_dollar_bang_during_rescue
  begin
    raise "boom"
  rescue
    e = $!
    assert(e.is_a?(Exception), "$! should be an Exception, got #{e.class}")
    assert_equal "boom", e.message
  end
end

def test_dollar_bang_nil_outside_rescue
  e = $!
  assert_equal nil, e
end

def test_bare_raise_reraises
  caught_inner = false
  caught_outer = nil
  begin
    begin
      raise "inner"
    rescue
      caught_inner = true
      raise   # bare re-raise of $!
    end
  rescue => e
    caught_outer = e.message
  end
  assert caught_inner, "inner rescue should fire"
  assert_equal "inner", caught_outer
end

# ---------- explicit re-raise of named exception ----------

def test_named_reraise
  caught = nil
  begin
    begin
      raise ArgumentError, "x"
    rescue ArgumentError => e
      raise e
    end
  rescue ArgumentError => e2
    caught = e2.message
  end
  assert_equal "x", caught
end

TESTS = [
  :test_dollar_bang_during_rescue,
  :test_dollar_bang_nil_outside_rescue,
  :test_bare_raise_reraises,
  :test_named_reraise,
]
TESTS.each { |t| run_test(t) }
report "GlobalsInRescue"
