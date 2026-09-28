# Fiber scheduler: per thread, closed when replaced / at thread end / before at_exit;
# current_scheduler is nil in a blocking fiber; Fiber.schedule calls #fiber
Warning[:experimental] = false
class S
  def initialize(n) = (@n = n)
  %i[block unblock io_wait fiber_interrupt].each { |m| define_method(m) { |*| } }
  def kernel_sleep(*a) = (puts "#{@n} kernel_sleep #{a.inspect}")
  def close = (puts "#{@n} close")
  def fiber(*a, &b) = (puts "#{@n} fiber #{a.inspect}"; Fiber.new(blocking: false, &b).tap(&:resume))
end
Fiber.set_scheduler(S.new(1))
p Fiber.scheduler.class, Fiber.current_scheduler
Fiber.schedule(1, 2) { |*x| p [:in, x, Fiber.current_scheduler.class, Fiber.blocking?]; sleep 0.1 }
Fiber.set_scheduler(S.new(2))
Fiber.set_scheduler(nil)
p Fiber.scheduler
t = Thread.new { Fiber.set_scheduler(S.new(3)); p Fiber.scheduler.class; :done }
p t.value, Fiber.scheduler
at_exit { puts "at_exit" }
Fiber.set_scheduler(S.new(9))
begin; Fiber.set_scheduler(Object.new); rescue => e; p e; end
t = Thread.new { Fiber.set_scheduler(S.new(8)); sleep 0.01; puts "thread body end" }
t.join
puts "main end"
