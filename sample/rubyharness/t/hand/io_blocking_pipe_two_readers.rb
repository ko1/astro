# Two green threads reading one blocking stdin pipe: the one that loses the race
# must wait in the scheduler, not in read(2) with the whole VM stopped.
require "rbconfig"
child = <<~'RB'
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  r = [$stdin, $stdin.dup].map { |io| Thread.new { io.read(1) } }
  sleep 0.3
  p Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 0.9
  p r.map(&:value).sort, $stdin.read
  r = [$stdin.dup, $stdin.dup].map { |io| Thread.new { io.gets } }
  sleep 0.3
  p :main_ran, r.map(&:value)
RB
cmd = "(printf x; sleep 1; printf yz) | #{RbConfig.ruby} -e '#{child}'"
p IO.popen(["sh", "-c", cmd], &:read)
p IO.popen(["sh", "-c", "(sleep 1; echo a) | #{RbConfig.ruby} -e '[STDIN, STDIN.dup].map { |i| Thread.new { i.gets } }.each(&:join); p :done'"], &:read)
# the child's stdin is a socket here: after close_write it is ready but empty (EOF)
p IO.popen([RbConfig.ruby, "-n", "-e", "print $_.size"], "r+") { |io| io.puts "ab"; io.close_write; io.read }
p IO.popen([RbConfig.ruby, "-e", "p $stdin.read(3), $stdin.read(3), $stdin.gets"], "r+") { |io| io.print "abcd"; io.close_write; io.read }
