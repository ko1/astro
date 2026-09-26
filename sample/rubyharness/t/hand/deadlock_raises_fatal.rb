# A deadlock raises `fatal` in the main thread (not exit from C): ensure and
# at_exit run with $! set, and rescue Exception can catch it.
require "rbconfig"
body = <<~'RB'
  $stdout.sync = true
  at_exit { puts "at_exit #{$!.class} #{$!&.message&.lines&.first.inspect}" }
  begin
    case ARGV[0]
    when "main" then Queue.new.pop
    when "join" then Thread.new { Queue.new.pop }.join
    when "mutex" then m = Mutex.new; m.lock; Thread.new { m.lock }.join
    when "cv" then m = Mutex.new; m.synchronize { ConditionVariable.new.wait(m) }
    when "rescue"
      begin; Thread.new { Queue.new.pop }.join; rescue Exception => e; puts "rescued #{e.class} #{e.class.superclass}"; end
      puts "after"
    end
  ensure
    puts "ensure"
  end
RB
%w[main join mutex cv rescue].each do |w|
  out = IO.popen([RbConfig.ruby, "-e", body, w], err: [:child, :out], &:read)
  p [w, out.lines.map(&:chomp).grep_v(/\A(\s|\*|\d+ threads|\z)/).map { _1.sub(/\A.*?: (No live)/, '\1') }, $?.exitstatus]
end
p [(fatal rescue $!.class), Object.constants.include?(:fatal)]
