# main (the top-level self) has its own singleton methods like CRuby; top-level
# def / alias / undef / define_method / include still target Object.
def foo = 1
alias bla foo
undef bla rescue p $!
p method(:foo).owner
define_method(:dm) { 2 } rescue p $!
p((method(:dm).owner rescue $!))
public def pub = 3
p Object.public_method_defined?(:pub)
private :pub
p Object.private_method_defined?(:pub)
X = 1
p Object.const_defined?(:X)
include Comparable
p Object.include?(Comparable)
p self.singleton_methods, self.inspect
class Foo; end
p Foo.name
$g = binding
p $g.receiver.to_s
p (singleton_class.instance_methods(false) - [:inspect, :to_s])
m = TOPLEVEL_BINDING.receiver
p m.singleton_methods, m.respond_to?(:ruby2_keywords, true), m.method(:ruby2_keywords).arity
p m.method(:to_s).owner == m.singleton_class
ruby2_keywords def r2k_top(*a) = a
p Hash.ruby2_keywords_hash?(r2k_top(k: 1).last)
# a break from the block given to .new makes .new return its value
class YieldsInInit; def initialize = yield; end
p [1, 2].map { |x| YieldsInInit.new { break x * 7 } }
