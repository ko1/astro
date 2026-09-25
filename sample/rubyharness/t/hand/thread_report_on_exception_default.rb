# Thread.report_on_exception= sets the default that new threads start with.
$stderr = $stdout
p Thread.report_on_exception
Thread.report_on_exception = false
p Thread.report_on_exception
t = Thread.new { raise "boom" }
p t.report_on_exception
begin; t.join; rescue => e; p e.message; end
Thread.report_on_exception = true
t2 = Thread.new { Thread.current.report_on_exception = false; raise "b2" }
begin; t2.join; rescue => e; p e.message; end
p Thread.new { Thread.current.report_on_exception }.value, Thread.main.report_on_exception
