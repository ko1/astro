# attr_accessor overridden on a SUPERCLASS's singleton (thor: class Base; extend ClassMethods; def attr_accessor(*) = no_commands { super })
class Ctx
  def initialize = @depth = 0
  def enter
    @depth += 1
    yield
  ensure
    @depth -= 1
  end
  def entered? = @depth.positive?
end
module CM
  def ctx = (@ctx ||= Ctx.new)
  def no_commands(&block) = ctx.enter(&block)
  def attr_accessor(*) = no_commands { super }
  def method_added(m)
    super(m)
    puts "added #{m} in_no_commands=#{ctx.entered?}"
  end
end
class Base; extend CM; end
class Sub < Base
  attr_accessor :tmpl
  def run; end
end
class Sub2 < Base
  def self.other = 1
  attr_accessor :t2
end
