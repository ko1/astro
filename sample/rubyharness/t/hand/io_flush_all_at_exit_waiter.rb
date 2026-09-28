require "rbconfig"
p IO.popen([RbConfig.ruby, "-e", 'IO.open(1).puts "x"; $stdout.print "s"']) { _1.read }.chars.sort  # CRuby's flush order is not fixed
r, w = IO.pipe
pid = spawn(RbConfig.ruby, "-e", 'IO.open(3).puts "fd3"', 3 => w); w.close
p r.gets; Process.wait(pid)
p IO.popen([RbConfig.ruby, "-e", 'IO.open(1).print "gone"; exit!(0)']) { _1.read }
t = Process.detach(spawn("true")); p t.class, t.pid.class, t.value.class, t.inspect.sub(/0x\h+/, "X"), t[:pid] == t.pid
class MyT < Thread; end
m = MyT.new { 1 }; m.join; p m.inspect.sub(/0x\h+/, "X").sub(/ \S+:\d+/, "")
p Thread.new { }.tap(&:join).inspect.sub(/0x\h+/, "X").sub(/ \S+:\d+/, "")
