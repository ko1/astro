# a binding taken in `def self.m` at the top level evals with the lexical cref (Object), not main's singleton
TOPLEVEL_BINDING.eval <<-RUBY
  def self.__tb = binding
  $tb = __tb
RUBY
$tb.eval("TEST_TB_CONST = [1]")
p Object.const_defined?(:TEST_TB_CONST), TEST_TB_CONST

# module_eval(String) from a `class << self` method sees that module's constants (dry-monads Do)
module DoLike
  VALUE = :found
  class << self
    def wrap(target) = target.module_eval("def m = VALUE")
  end
end
anon = Module.new
DoLike.wrap(anon)
o = Object.new.extend(anon)
p((o.m rescue $!.class))

# the first srand returns the (Integer) seed the default generator started from
s = srand
p s.is_a?(Integer), srand(s).is_a?(Integer)
