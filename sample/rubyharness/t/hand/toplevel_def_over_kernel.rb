# A top-level def is Object's method, so it beats Kernel's of the same name
# (ruby-bench's object-new-no-escape defines `test`, which Kernel#test shadowed).
# Run under CRuby and koruby and diff.
def test = :mine
p ["bare call", test]
p ["send", send(:test)]
p ["method", method(:test).call, method(:test).owner, Object.instance_method(:test).owner]
class Q; def r = test; end
p ["from an instance", Q.new.r]
module Kernel; def zzq = :k; end
def zzq = :o
p ["after a Kernel def", zzq, Object.instance_method(:zzq).owner]
def open(*) = :o
p ["open", open]
p ["Kernel.test still there", Kernel.test(?e, __FILE__)]
