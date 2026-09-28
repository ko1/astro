class RM < Mutex
  def initialize; @c = Mutex.new; @h = Hash.new(0); super; end
  def lock = (@c.synchronize { @h[Thread.current] += 1 }; super)
  def unlock = (@c.synchronize { @h[Thread.current] -= 1 }; super)
  def synchronize
    lock; yield
  ensure
    unlock
  end
  attr_reader :c
end
m = RM.new
p m.class, m.c.class, m.instance_variables
m.synchronize { p :in }
t = Thread.new { m.synchronize { :t } }; p t.value
p m.c.class
class M2 < Mutex; end; p M2.new.class, (M2.new(1) rescue $!.class); class C2 < ConditionVariable; def initialize(x) = (@x = x; super()); attr_reader :x; end; p C2.new(5).x
