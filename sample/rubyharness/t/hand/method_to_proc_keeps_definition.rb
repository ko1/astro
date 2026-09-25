# Method#to_proc (and &obj.method(:m) into define_method) calls the definition the
# Method captured, not whatever the name resolves to later (rspec-support's
# Source::File copies File.read this way, then specs stub File.read).
class K; def self.hi(x = 1) = "orig#{x}"; end
class W; class << self; define_method(:hi, &K.method(:hi)); end; end
m = K.method(:hi); pr = m.to_proc
K.singleton_class.send(:define_method, :hi) { |x = 1| "stubbed" }
p W.hi, pr.call, pr.call(2), m.call, [3].map(&m), pr.arity, pr.lambda?, K.hi
def blk(&b) = b.call(9)
p blk(&m)
o = Object.new; def o.each_it(&b) = [1, 2].each(&b)
sq = 5.method(:+); p [1, 2].map(&sq)
