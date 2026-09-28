# a green thread sleeping < 1 ms at a time must not spin the CPU (the pump's
# timeout was truncated to whole ms, i.e. poll(0) until the deadline)
t0 = Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID)
th = Thread.new { loop { sleep 0.001 } }
sleep 0.3
th.kill
p Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID) - t0 < 0.1
w0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
sleep 0.0003
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - w0 < 0.05
