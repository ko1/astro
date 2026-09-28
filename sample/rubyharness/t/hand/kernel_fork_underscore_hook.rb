# Kernel#fork goes through Process._fork, so a prepended hook runs in parent and child
module ForkHook; def _fork; r = super; $stdout.write(r == 0 ? "" : "parent-hook\n"); r; end; end
Process.singleton_class.prepend(ForkHook)
r, w = IO.pipe
pid = fork { r.close; w.write("child-ran"); w.close; exit!(0) }
w.close
p r.read, pid.is_a?(Integer)
Process.wait(pid)
p $?.success?
