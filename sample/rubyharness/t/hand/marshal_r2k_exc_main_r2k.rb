# Marshal keeps the ruby2_keywords mark (:K), loads an Exception without calling
# its #initialize, and names an undefined path the way CRuby does; main has
# ruby2_keywords for top-level defs.
ruby2_keywords def mr_cap(*a) = a
h = mr_cap(a: 42).last
p Hash.ruby2_keywords_hash?(h), Marshal.dump(h)
h2 = Marshal.load(Marshal.dump(h))
p Hash.ruby2_keywords_hash?(h2), h2
def mr_kw(a:) = a
p mr_kw(*[h2])
h3 = mr_cap(b: 1).last; h3.instance_variable_set(:@x, 1)
d = Marshal.dump([h3, h3]); p d
l = Marshal.load(d); p Hash.ruby2_keywords_hash?(l[0]), l[0].equal?(l[1]), l[0].instance_variable_get(:@x)
p Hash.ruby2_keywords_hash?(Marshal.load(Marshal.dump({a: 1})))
class MrErr < StandardError
  def initialize(err) = (puts "init #{err.inspect}"; super(err.message))
end
e = MrErr.new(RuntimeError.new("boom"))
e2 = Marshal.load(Marshal.dump(e)); p e2, e2.message, e2.class
[RuntimeError.new("x"), ArgumentError.new, KeyError.new("k"), NameError.new("n"), StopIteration.new("s"), SystemExit.new(3, "bye")].each do |x|
  y = Marshal.load(Marshal.dump(x)); p [y.class, y.message]
end
c = begin; begin; raise "inner"; rescue; raise ArgumentError, "outer"; end; rescue => ex; ex; end
cc = Marshal.load(Marshal.dump(c)); p cc.message, cc.cause&.message
module MrM; end
["o:\x0EMrM::N::Foo\x00", "o:\x0DMrM::Foo\x00", "o:\x0CNoSuch9\x00", "o:\x11NoSuch9::A::B\x00"].each do |s|
  begin; Marshal.load("\x04\b".b + s.b); rescue => err; p err.message; end
end
