# Bare calls that reach a define_method'd method (instance and class self) or a global builtin from a class /
# builtin self, through the site's cache: redefinition, undef, private, singleton, arity errors and blocks
class Cfg
  define_method(:greet) { |n = 1| "hi#{n}" }
  define_method(:twice) { |x| x * 2 }
  def run(i) = [greet, greet(i), twice(i)]
  class << self
    define_method(:logger) { @logger ||= :log }
    define_method(:routes) { |*a| a }
  end
  def self.use(i) = [logger, routes(i, i), Integer("4#{i}"), Array(i), format("%02d", i)]
  def self.bad = twice
end
o = Cfg.new
3.times do |i|
  GC.start if i == 1
  p o.run(i), Cfg.use(i)
end
p Process.clock_gettime(Process::CLOCK_MONOTONIC).is_a?(Float)
class Cfg; define_method(:greet) { |n = 9| "yo#{n}" }; end
p o.run(5)
class << Cfg; define_method(:logger) { :newlog }; end
p Cfg.use(6)
class << Cfg; define_method(:Array) { |x| [:mine, x] }; end
p Cfg.use(7)
class << Cfg; undef_method :Array; end
begin
  Cfg.use(8)
rescue NoMethodError => e
  p e.class, e.name
end
begin
  Cfg.bad
rescue NameError => e
  p e.class, e.name
end
class Cfg; private :twice; end
p o.run(3)
begin
  o.twice(1)
rescue NoMethodError => e
  p e.class
end
class Cfg; define_method(:twice) { |x| raise ArgumentError, "boom #{x}" }; end
begin
  o.run(2)
rescue ArgumentError => e
  p e.message
end
p "str".then { |s| s.instance_eval { [Integer("12"), Array(nil), format("%s!", self)] } }

class K
  class << self; define_method(:boom) { raise "x" }; end
  def self.a = Integer("zz")
  def self.b = boom
  def self.c = caller(0, 2)
  class << self; define_method(:cl) { c }; end
  def self.d = cl
end
begin; K.a; rescue => e; p e.backtrace[0, 3], e.backtrace_locations&.first&.label; end
begin; K.b; rescue => e; p e.backtrace[0, 3]; end
p K.d
