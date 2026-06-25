# class instance variables + general `class << self` body (rubyspec follow-up)
class Counter
  @count = 0
  def self.incr; @count += 1; end
  def self.count; @count; end
end
Counter.incr; Counter.incr; Counter.incr
p Counter.count

class Reg
  class << self
    attr_accessor :items
    def make; "made"; end
  end
  @items = []
end
Reg.items = [1, 2, 3]
p Reg.items
p Reg.make
p Reg.instance_variable_get(:@items)
Reg.instance_variable_set(:@extra, "x")
p Reg.instance_variable_get(:@extra)

# class << recv for a specific object
obj = Object.new
class << obj
  def special; "special!"; end
end
p obj.special

# class << self with alias_method
class D2
  def self.orig; "orig"; end
  class << self
    alias_method :aliased, :orig
  end
end
p D2.aliased

# class << nil / immediates
class << nil
  def koruby_nil_probe; "from nilclass"; end
end
p nil.koruby_nil_probe
p(begin; class << 1; end; rescue TypeError; "TypeError"; end)
