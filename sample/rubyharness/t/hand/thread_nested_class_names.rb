# Mutex / Queue / SizedQueue / ConditionVariable / Monitor are Thread::X (the top-level
# names are aliases); their C / prelude internals stay out of inspect and reflection.
require "yaml"
p [Mutex, Queue, SizedQueue, ConditionVariable, Monitor, ClosedQueueError].map(&:name), Mutex.equal?(Thread::Mutex)
p Mutex.new.inspect.sub(/0x\h+/, "X"), Thread::ConditionVariable.new.to_s.sub(/0x\h+/, "X"), Queue.new.instance_variables, Monitor.new.instance_variables
class Q2 < Queue; def initialize = (super; @__mine = 1); end
p Q2.new.instance_variables
o = Object.new; o.instance_variable_set(:@__x, 1); p o.instance_variables
puts YAML.dump(Queue.new), YAML.dump(Mutex.new)
p YAML.load(YAML.dump(Queue.new), permitted_classes: [Thread::Queue]).class
