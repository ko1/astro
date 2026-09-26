# $_ and $? are thread-local: a new thread starts with nil and its own writes
# do not reach the main thread.
$_ = "main"
t = Thread.new { a = $_; $_ = "thr"; [a, $_] }
p t.value, $_
system("exit 3")
t2 = Thread.new { a = $?; system("exit 5"); [a, $?.exitstatus] }
p t2.value, $?.exitstatus
q = Queue.new; go = Queue.new
t3 = Thread.new { system("exit 7"); q << $?.exitstatus; go.pop; $?.exitstatus }
p q.pop, $?.exitstatus
go << :go
p t3.value
