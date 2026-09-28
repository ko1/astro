# catch / throw per green thread (the CTX is shared: another thread's catch popped ours); "uncaught throw :tag"
q1 = Queue.new; q2 = Queue.new
t = Thread.new do
  catch(:b) do
    q1 << :b_in
    q2.pop            # main pushes its catch, then pops it while we are still inside
    throw :b, :from_b
  end
end
r = catch(:a) do
  q1.pop
  :a_done
end
q2 << :go
p r, t.value
t2 = Thread.new { catch(:x) { Thread.pass; throw :x, 1 } }
p catch(:y) { Thread.pass; t2.value }
begin; throw "x"; rescue => e; p e.message; end
p Fiber.current.inspect.sub(/0x\h+/, "X"), Thread.current.inspect[/0x\h+/].size
