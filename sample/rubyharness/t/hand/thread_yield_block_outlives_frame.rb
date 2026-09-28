def deep(n) = n.zero? ? [Object.new, 1, 2, 3] : deep(n - 1)
def wrap = yield
def st3
  th = Thread.new { yield }
  sleep 0.001 until th.stop?
  th
end
def run3
  q2 = nil; th2 = nil
  wrap { s = "G"; q = Queue.new; th2 = st3 { q.pop; s }; q2 = q }
  deep(80); q2.push 1; p th2.value
end
run3
class T
  def initialize = (@threads = [])
  def start_server
    th = Thread.new { yield }
    @threads << th
    sleep 0.001 until th.stop?
  end
  def deep(n) = n.zero? ? [Object.new, 1, 2, 3] : deep(n - 1)
  def wrap = yield
  def run
    wrap do
      server = "SERVER"
      q = Queue.new
      start_server do
        begin
          q.pop
        ensure
          p [:in_thread, server]
        end
      end
      @q = q
    end
    deep(80)
    @q.push(1)
    @threads.each(&:join)
  end
end
T.new.run
