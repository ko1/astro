# Klass.new with a block: the block is forwarded to #initialize (which may
# yield to it), and for a class whose #initialize is the default (CFUNC)
# Object#initialize the block is simply ignored — this used to SIGBUS because
# the block dispatch path invoked a CFUNC initialize as if it were ISEQ. vs ruby.
class Plain; end
p Plain.new { raise "never called" }.class

class Init
  def initialize
    yield 42 if block_given?
  end
end
Init.new { |x| p x }
p Init.new.class

class WithArgs
  def initialize(a, b)
    @sum = a + b
    yield @sum if block_given?
  end
  def sum; @sum; end
end
p WithArgs.new(3, 4) { |s| p s }.sum
