# report_on_exception は thread の inspect 行を前置し、$stderr 経由で出す。
# stderr は unbuffered なので、ここまでの stdout を先に出しておく。
$stdout.flush
def raiser = raise(RuntimeError, "roe test")
t = Thread.new do
  Thread.current.report_on_exception = true
  raiser
end
Thread.pass while t.alive?
begin; t.join; rescue RuntimeError => e; p [:joined, e.message]; end
