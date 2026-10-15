# Kernel#syscall / set_trace_func, Process.getpgid via #to_int, open("|cmd") is a file name,
# features provided from the start (fiber/pathname/monitor/io/wait) (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:syscall_vis) { [Kernel.private_instance_methods.include?(:syscall), Kernel.respond_to?(:syscall)] }
t(:syscall_getpid) { syscall(39) == Process.pid }
t(:stf_nil) { set_trace_func(nil) }
t(:stf_type) { set_trace_func(1) }
t(:getpgid_to_int) { o = Object.new; def o.to_int = Process.pid; Process.getpgid(o) == Process.getpgrp }
t(:getpgid_bad) { Process.getpgid("x") }
t(:open_pipe) { open("|echo ok") { } }
t(:monitor) { m = Monitor.new; m.synchronize { :in } }
t(:monitor_mixin) { Class.new { include MonitorMixin }.new.respond_to?(:mon_synchronize) }
