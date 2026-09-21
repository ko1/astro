# `random:` takes ANY object and CRuby dispatches #rand on it publicly.
# koruby only accepted a T_OBJECT, so a Module (ffaker passes FFaker::Random)
# silently fell back to the default generator and the results stopped being
# repeatable; nil / Integer / Symbol did the same instead of raising.
a = (1..10).to_a
module M
  def self.reset!
    @r = Random.new(99)
  end
  def self.rand(n = nil)
    @r ||= Random.new(99)
    n ? @r.rand(n) : @r.rand
  end
end
M.reset!; x = Array.new(5) { a.sample(random: M) }
M.reset!; y = Array.new(5) { a.sample(random: M) }
p [x, y, x == y]
M.reset!; s1 = a.shuffle(random: M)
M.reset!; s2 = a.shuffle(random: M)
p [s1, s1 == s2]
M.reset!; p a.sample(3, random: M)

class Fixed
  def initialize
    @n = 0
  end
  def rand(m)
    (@n += 1) % m
  end
end
p [a.sample(random: Fixed.new), a.shuffle(random: Fixed.new), a.sample(3, random: Fixed.new)]

# Kernel#rand is private, so a plain object is a NoMethodError, not a fallback
[nil, 5, "x", :sym, Object, Kernel, 1.5].each do |v|
  r = begin; a.sample(random: v); :ok; rescue => e; [e.class, e.message]; end
  s = begin; a.shuffle(random: v); :ok; rescue => e; [e.class, e.message]; end
  t = begin; a.sample(3, random: v); :ok; rescue => e; [e.class, e.message]; end
  p [v.inspect, r, s, t]
end

# an explicit Random still takes the fast path and stays bit-exact
p [a.shuffle(random: Random.new(4)), a.sample(4, random: Random.new(4))]

# #rand returning a non-Integer / out-of-range value
class Bad
  def rand(_m) = 10_000
end
p (a.sample(random: Bad.new) rescue [$!.class, $!.message])
class Str
  def rand(_m) = "2"
end
p (a.sample(random: Str.new) rescue [$!.class, $!.message])
