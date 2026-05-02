require_relative "../../test_helper"

# attr_reader / attr_writer / attr_accessor.

class Box
  attr_reader   :ro
  attr_writer   :wo
  attr_accessor :rw
  def initialize
    @ro = "ro_init"
    @rw = "rw_init"
  end
  def wo_inspect; @wo; end
end

def test_attr_reader_returns_ivar
  assert_equal "ro_init", Box.new.ro
end

def test_attr_writer_sets_ivar
  b = Box.new
  b.wo = "set"
  assert_equal "set", b.wo_inspect
end

def test_attr_accessor_both_ways
  b = Box.new
  assert_equal "rw_init", b.rw
  b.rw = "updated"
  assert_equal "updated", b.rw
end

# ---------- multiple attrs in one call ----------

class Multi
  attr_accessor :a, :b, :c
end

def test_multi_attr_accessor
  m = Multi.new
  m.a = 1; m.b = 2; m.c = 3
  assert_equal 1, m.a
  assert_equal 2, m.b
  assert_equal 3, m.c
end

# ---------- attr returns nil before set ----------

class Maybe
  attr_accessor :x
end

def test_attr_default_nil
  assert_equal nil, Maybe.new.x
end

# ---------- writer returns rhs ----------

def test_writer_returns_rhs
  m = Maybe.new
  result = (m.x = 99)
  assert_equal 99, result
end

TESTS = [
  :test_attr_reader_returns_ivar,
  :test_attr_writer_sets_ivar,
  :test_attr_accessor_both_ways,
  :test_multi_attr_accessor,
  :test_attr_default_nil,
  :test_writer_returns_rhs,
]
TESTS.each { |t| run_test(t) }
report "AttrAccessors"
