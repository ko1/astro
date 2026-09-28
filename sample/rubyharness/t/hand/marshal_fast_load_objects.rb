# Marshal.load in C for plain objects of vetted classes: shared refs, cycles, a non-@ ivar name is
# kept hidden (not a NameError), a user allocate is not called, proc / freeze: still honoured
module NS; class Inner; def initialize(v) = (@v = v); end; end
class Pt; def initialize(x, y) = (@x = x; @y = y); attr_reader :x, :y; end
def rt(o) = Marshal.load(Marshal.dump(o))
def desc(o, seen = {}.compare_by_identity)
  case o
  when Pt, NS::Inner
    return "#<ref>" if seen[o]
    seen[o] = true
    "#{o.class}(" + o.instance_variables.map { |iv| "#{iv}=#{desc(o.instance_variable_get(iv), seen)}" }.join(",") + ")"
  when Array then "[" + o.map { |e| desc(e, seen) }.join(",") + "]"
  else o.inspect
  end
end
2.times do |round|
  a = rt(Pt.new(NS::Inner.new([1, :s, "é"]), {k: 1.5}))
  puts "#{round} #{desc(a)}"
  sh = NS::Inner.new(9); b = rt([sh, Pt.new(sh, sh)])
  puts "#{round} shared #{b[0].equal?(b[1].x)} #{b[1].x.equal?(b[1].y)}"
  cy = Pt.new(nil, 1); cy.instance_variable_set(:@x, cy); c2 = rt(cy)
  puts "#{round} cycle #{c2.x.equal?(c2)}"
end
bad = Marshal.dump(Pt.new(1, 2)).sub("\x07@x".b, "\x07zx".b)
begin; p Marshal.load(bad).instance_variables; rescue => e; p e.class; end
unk = Marshal.dump(Pt.new(1, 2)).sub(":\x07Pt".b, ":\x07Qq".b)
begin; p Marshal.load(unk); rescue => e; p e.class, e.message; end
class Pt; def self.allocate = (puts "custom allocate"; super); end
p rt(Pt.new(3, 4)).y
p Marshal.load(Marshal.dump(Pt.new(5, 6)), ->(x) { x.is_a?(Integer) ? x * 10 : x }).y
p Marshal.load(Marshal.dump(Pt.new("f", 6)), freeze: true).x.frozen?
