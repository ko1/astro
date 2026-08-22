# String.new encoding (CRuby: no source → ASCII-8BIT) + marshal fixes
p String.new.encoding
p String.new("abc").encoding
p String.new("あ").encoding
p Class.new(String).new.encoding
k = Class.new(String); p k.new("x").encoding
p Marshal.dump(:sym)
p Marshal.dump([:a, :a])
# concurrent require: 2nd thread waits for the 1st
File.write("/tmp/claude-1000/cr_a.rb", "$cr_order << :loaded; sleep 0.05")
$cr_order = []
ts = 2.times.map { Thread.new { require "/tmp/claude-1000/cr_a.rb" } }
rets = ts.map(&:value)
p $cr_order
p rets.sort_by(&:to_s)
