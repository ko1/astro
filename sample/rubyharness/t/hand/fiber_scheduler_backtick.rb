# `cmd` in a non-blocking fiber waits for the pipe in scheduler.io_wait, so two
# subprocesses overlap; $? is set; Fiber#resume with no values passes no block args
Warning[:experimental] = false
class MiniSched
  def initialize = (@readable = {}; @log = [])
  attr_reader :log
  def block(*) = nil
  def unblock(*) = nil
  def fiber_interrupt(*) = nil
  def kernel_sleep(*) = nil
  def io_wait(io, events, _tmo)
    @log << :io_wait
    @readable[io] = Fiber.current
    Fiber.yield
    events
  end
  def fiber(&b) = Fiber.new(blocking: false, &b).tap(&:resume)
  def close
    until @readable.empty?
      ready, = IO.select(@readable.keys)
      ready.each { |io| @readable.delete(io).resume }
    end
  end
end
s = MiniSched.new
out = []
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
Fiber.set_scheduler(s)
Fiber.schedule { out << `sleep 0.3; echo a`; out << $?.exitstatus }
Fiber.schedule { out << `sleep 0.3; echo b; exit 3`; out << $?.exitstatus }
Fiber.set_scheduler(nil)
dt = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
p out.sort_by(&:to_s), s.log.uniq, dt < 0.55
p `echo plain`, Fiber.new { |*x| x }.resume, Fiber.new { |*x| x }.resume(1)
