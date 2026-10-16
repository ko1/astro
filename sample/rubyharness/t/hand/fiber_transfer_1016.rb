# Fiber#transfer: a finished transferred fiber returns to the resumed one, transfer to the root
# from deep inside, transfer to self; Kernel#public_send's own frame in argument errors (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:back_to_resumed) do
  states = []
  fiber = Fiber.new { states << :fiber; [Fiber.current == fiber, Fiber.current.alive?] }
  fiber2 = Fiber.new { states << :fiber2; fiber.transfer; states << :flunk2 }
  fiber3 = Fiber.new { states << :fiber3; r = fiber2.transfer; states << [:fiber3_back, r] }
  fiber3.resume
  states
end
t(:finished_value) { f1 = Fiber.new { :f1_done }; f2 = Fiber.new { [:f2_back, f1.transfer] }; [f2.resume, f2.alive?] }
t(:dead_raise) { f3 = Fiber.new { raise "boom" }; Fiber.new { f3.transfer; :no }.resume }
t(:self_transfer) do
  states = []
  fiber = Fiber.new { states << :start; states << fiber.transfer(:v); states << :end }
  fiber.transfer
  fiber = Fiber.new { states << :start2; fiber.transfer; states << :end2 }
  fiber.resume
  states << Fiber.current.transfer(1, 2)
end
t(:root_and_back) do
  states = []
  f1 = Fiber.new { |f0| states << 0; states << f0.transfer(1); 3 }
  states << f1.transfer(Fiber.current)
  states << f1.transfer(2)
end
t(:root_from_deep) do
  root = Fiber.current
  f1 = Fiber.new { root.transfer(:from_f1) }
  f2 = Fiber.new { f1.resume }
  [f2.transfer, (f2.raise(RuntimeError, "expected") rescue $!.message)]
end
t(:raise_transferring) { root = Fiber.current; fiber = Fiber.new { root.transfer }; fiber.transfer; fiber.raise "msg" }
t(:thread_root) do
  Thread.new do
    io = Fiber.new { |calling| calling.transfer }
    io.transfer(Fiber.current)
    v = Object.new
    io.transfer(v).equal?(v)
  end.value
end
t(:yield_still) { g = Fiber.new { Fiber.yield 1; 2 }; [g.resume, g.resume, g.alive?] }
t(:public_send_none) { begin; public_send(); rescue ArgumentError => e; e.backtrace[0][/'[^']+'/]; end }
t(:public_send_bad) { begin; public_send(Object.new); rescue TypeError => e; e.backtrace[0][/'[^']+'/]; end }
t(:send_none) { begin; send(); rescue ArgumentError => e; e.backtrace[0][/'[^']+'/]; end }
t(:public_send_private) { c = Class.new { private def q; end; protected def r; end }; [:q, :r].map { |m| begin; c.new.public_send(m); rescue NoMethodError => e; e.backtrace[0][/'[^']+'/]; end } }
def ps_top; end
t(:public_send_top) { begin; public_send(:ps_top); rescue NoMethodError => e; e.backtrace[0][/'[^']+'/]; end }
