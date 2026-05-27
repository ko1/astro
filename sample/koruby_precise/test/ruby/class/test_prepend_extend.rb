require_relative "../../test_helper"

module Logger
  def hi; "logged[#{super}]"; end
end

class Loud
  prepend Logger
  def hi; "Loud-hi"; end
end

def test_prepend_overrides_class_method
  # Prepend's `hi` runs first; it calls `super` which finds Loud's `hi`.
  assert_equal "logged[Loud-hi]", Loud.new.hi
end

class K1; end
module Helper; def help; "helped"; end; end
K1.extend(Helper)

def test_extend_adds_class_methods
  assert_equal "helped", K1.help
end

def test_ancestors_includes_prepended
  anc = Loud.ancestors
  assert_equal true, anc.index(Logger) < anc.index(Loud)
end

TESTS = [:test_prepend_overrides_class_method, :test_extend_adds_class_methods, :test_ancestors_includes_prepended]
TESTS.each { |t| run_test(t) }
report "PrependExtend"
