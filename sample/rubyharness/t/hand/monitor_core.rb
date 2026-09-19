# Monitor は CRuby では monitor.c の C 実装 = core。stdlib の monitor.rb は
# MonitorMixin だけを足す。
m = Monitor.new
p m.synchronize { m.synchronize { m.mon_owned? } }
p m.mon_locked?
p m.try_enter
p m.mon_owned?
m.exit
p m.mon_locked?
begin; m.exit; rescue ThreadError => e; p e.message; end

cond = m.new_cond
ready = false
t = Thread.new { m.synchronize { ready = true; cond.signal } }
m.synchronize { cond.wait_until { ready } }
t.join
p ready

require "monitor"
buf = []
buf.extend(MonitorMixin)
p buf.mon_synchronize { 42 }
class Sync
  include MonitorMixin
end
p Sync.new.synchronize { :ok }
p MonitorMixin::ConditionVariable.equal?(Monitor::ConditionVariable)
