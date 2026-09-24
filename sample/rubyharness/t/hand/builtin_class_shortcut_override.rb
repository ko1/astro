# Several builtin class methods are dispatched by a C fast path that runs BEFORE
# the method table is consulted.  A definition on the class object itself — a
# `def self.x`, or an rspec `allow(Klass).to receive(:x)` — has to win over it.
# `Kernel.<global fn>` was the first one found (simplecov stubs Kernel.exec);
# Fiber and Range had the same hole.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

# --- Fiber: current / blocking? / [] / []= -------------------------------
t(:fiber_plain)  { [Fiber.current.class, Fiber.blocking?] }
class << Fiber
  def current(*a) = [:mine, a]
  def blocking?(*a) = [:mine_b, a]
end
t(:fiber_fixed)  { [Fiber.current, Fiber.blocking?] }
t(:fiber_splat)  { a = []; [Fiber.current(*a), Fiber.blocking?(*a)] }
t(:fiber_send)   { Fiber.send(:current) }
# (CRuby's Fiber.current IS a real singleton method, so removing it there is
# permanent; koruby's C fast path would come back.  Not comparable — skipped.)

# --- Range.new ----------------------------------------------------------
t(:range_plain)  { [Range.new(1, 3).to_a, Range.new(1, 3, true).to_a] }
class << Range; def new(*a) = [:mine, a]; end
t(:range_fixed)  { Range.new(1, 3) }
t(:range_splat)  { a = [1, 3]; Range.new(*a) }
t(:range_sub)    { c = Class.new(Range); (c.new(1, 3) rescue $!.class) }
class << Range; remove_method :new; end
t(:range_back)   { [Range.new(1, 3).to_a, Range.new(1, 3, true).to_a] }

# a Range SUBCLASS with its own `new` keeps working either way
class MyRange < Range; def self.new(a, b) = super(a, b, false); end
t(:range_subnew) { MyRange.new(1, 3).to_a }

# --- and the ordinary paths are untouched -------------------------------
t(:range_lit)    { (1..3).to_a }
t(:fiber_run)    { f = Fiber.new { Fiber.yield 1; 2 }; [f.resume, f.resume] }
t(:kernel_fmt)   { Kernel.format("%d-%d", 1, 2) }
