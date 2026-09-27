# A method's &b Proc called on another thread or fiber and breaking is a LocalJumpError
# (reason :break) raised at the call, which DRb rescues to carry the break back.
def t = (yield rescue [$!.class, $!.message, ($!.reason rescue nil), ($!.exit_value rescue nil)])
def get(&b) = b
def call_in_thread(&b) = Thread.new { t { b.call(4) } }.value
p call_in_thread { |x| break x * 2 }
def call_in_fiber(&b) = Fiber.new { t { b.call(5) } }.resume
p call_in_fiber { |x| break x * 3 }
def call_here(&b) = b.call(6)
p call_here { |x| break x + 1 }
def yield_in_thread(&b) = Thread.new { t { [1].each(&b) } }.value
p yield_in_thread { break :y }
def each_here(&b) = [1, 2, 3].each(&b)
p each_here { |x| break x * 10 if x == 2 }
p [1, 2].map { |x| call_here { |y| break y + x } }
r = Thread.new { call_here { |x| break :in_thread } }.value; p r
q = Queue.new; th = Thread.new { b = q.pop; t { b.call } }; def pass(q, &b) = (q << b; sleep 0.05; :passed); p pass(q) { break :x }; p th.value
