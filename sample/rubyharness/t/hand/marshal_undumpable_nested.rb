# Marshal refuses the classes CRuby implements as C data (StringIO, Queue, Monitor, Dir ...) at any
# depth, not only at the top (parallel wraps an undumpable exception on that TypeError)
Warning[:experimental] = false
require "stringio"; require "monitor"; require "set"; require "strscan"
{ "StringIO" => -> { StringIO.new("x") }, "Mutex" => -> { Mutex.new }, "Queue" => -> { Queue.new }, "SizedQueue" => -> { SizedQueue.new(1) },
  "CondVar" => -> { ConditionVariable.new }, "Monitor" => -> { Monitor.new }, "Set" => -> { Set[1] }, "StringScanner" => -> { StringScanner.new("a") },
  "Thread" => -> { Thread.current }, "Proc" => -> { proc {} }, "Method" => -> { 1.method(:+) }, "Binding" => -> { binding },
  "IO" => -> { $stdout }, "File::Stat" => -> { File.stat("/") }, "Random" => -> { Random.new(1) }, "Enumerator" => -> { [1].each },
  "Dir" => -> { Dir.new("/") }, "Encoding" => -> { Encoding::UTF_8 }, "Fiber" => -> { Fiber.new {} }, "Ractor::Port" => -> { Ractor::Port.new } }.each do |name, mk|
  r = begin; Marshal.dump(mk.call); "ok"; rescue TypeError => e; e.message.sub(/0x\h+/, "X"); end
  puts "#{name}: #{r}"
end
class MyE < StandardError; def initialize(o = nil) = (super(); @object = o); end
e = MyE.new([1, 2]); e.set_backtrace(["a:1"])
x = Marshal.load(Marshal.dump(e))
p x.class, x.message, x.instance_variable_get(:@object), x.backtrace
p Marshal.dump(e).bytesize > 0
require "stringio"
begin; Marshal.dump(MyE.new(StringIO.new)); p :dumped; rescue TypeError => err; p err.message; end
