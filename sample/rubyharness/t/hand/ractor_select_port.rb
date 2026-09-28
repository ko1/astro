# Ractor.select raises ClosedError for a closed empty port (not swallowed by `loop`) and does not
# spin while waiting; Port#inspect names the owner and a per-ractor id (default port 0)
$VERBOSE = nil
p1 = Ractor::Port.new; p2 = Ractor::Port.new
r = Ractor.new(p2) { |pt| sleep 0.05; pt << :hi; :done }
p Ractor.select(p1, p2)
p2.close
begin; Ractor.select(p1, p2); rescue Ractor::ClosedError => e; p e.class; end
t0 = Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID)
th = Thread.new { Ractor.select(Ractor::Port.new) }
sleep 0.3; th.kill
p Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID) - t0 < 0.15
p r.value
p Ractor::Port.new; r = Ractor.new { [Ractor::Port.new, Ractor.current.default_port].map(&:inspect) }; p r.value, Ractor::Port.new, r.default_port
r2 = Ractor.new { 2 }; r2.join; p Ractor::Port.new, Ractor.current.default_port
