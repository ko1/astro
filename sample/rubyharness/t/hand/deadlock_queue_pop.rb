# Every thread waiting forever on a Queue / ConditionVariable / Mutex#sleep is a
# deadlock (fatal, exit 1); a bare `sleep`, or a wait something can end, is not.
require 'rbconfig'
[
  "Queue.new.pop",
  "m = Mutex.new; cv = ConditionVariable.new; m.synchronize { cv.wait(m) }",
  "m = Mutex.new; Thread.new { m.synchronize { m.sleep } }.join",
  "q = Queue.new; Thread.new { sleep 0.1; q << 1 }; print q.pop",
].each do |code|
  out = IO.popen([RbConfig.ruby, "-e", code], err: [:child, :out], &:read)
  p [out.include?("No live threads left. Deadlock?") ? :deadlock : out, $?.exitstatus]
end
