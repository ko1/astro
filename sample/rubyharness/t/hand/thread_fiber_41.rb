# Fiber.current_scheduler は非 blocking な fiber のときだけ scheduler を返す。
class Sched
  def block(*) = nil
  def unblock(*) = nil
  def kernel_sleep(*) = nil
  def io_wait(*) = nil
  def fiber_interrupt(*) = nil
end
Fiber.set_scheduler(Sched.new)
p [Fiber.blocking? ? true : false, Fiber.current_scheduler.nil?, Fiber.scheduler.nil?]
f = Fiber.new(blocking: false) { [Fiber.blocking? ? true : false, Fiber.current_scheduler.nil?] }
p f.resume
g = Fiber.new(blocking: true) { [Fiber.blocking? ? true : false, Fiber.current_scheduler.nil?] }
p g.resume
Fiber.blocking { p [:in_blocking, Fiber.current_scheduler.nil?] }
Fiber.set_scheduler(nil)
p Fiber.current_scheduler.nil?

# report_on_exception は thread の inspect 行を前置し、$stderr 経由で出す。
# stderr は unbuffered なので、ここまでの stdout を先に出しておく。
$stdout.flush
def raiser = raise(RuntimeError, "roe test")
t = Thread.new do
  Thread.current.report_on_exception = true
  raiser
end
Thread.pass while t.alive?
begin; t.join; rescue RuntimeError => e; p [:joined, e.message]; end
