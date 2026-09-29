# Hash.ruby2_keywords_hash / ? are real methods (respond_to? / method see them;
# grape asks before calling).  Run under CRuby and koruby and diff.
p Hash.respond_to?(:ruby2_keywords_hash), Hash.respond_to?(:ruby2_keywords_hash?)
h = Hash.ruby2_keywords_hash({a: 1}); p Hash.ruby2_keywords_hash?(h), Hash.ruby2_keywords_hash?({a: 1})
1000.times { Hash.ruby2_keywords_hash?(h) }
p Hash.method(:ruby2_keywords_hash).arity
x = Hash.public_send(:ruby2_keywords_hash, {b: 1}); p Hash.send(:ruby2_keywords_hash?, x)
p((Hash.ruby2_keywords_hash(1) rescue $!))
class H2 < Hash; end
p((H2.ruby2_keywords_hash?({}) rescue $!))
def use(m, *args) = m.new(:app, *args)
ruby2_keywords(:use)
args = [{a: 1}]
args[-1] = Hash.ruby2_keywords_hash(args[-1])
p Hash.ruby2_keywords_hash?(args[-1])
use(M, *args)
h = Hash.ruby2_keywords_hash({b: 2}); p h, h.frozen?
