# Data.define on a Data subclass makes a subclass of it (net-imap ConnectionState); Data.new rejects
# keywords mixed with positional values and too many values, maps fewer positional values to the first members
class CS < Data
  def self.define(sym, *a) = super(*a).tap { |k| k.const_set(:NAME, sym) }
  def hi = self.class::NAME
end
A = CS.define(:auth)
p A.superclass, A.new.hi, A.ancestors.take(3)
class SS < Struct; end
B = SS.new(:x)
p B.superclass, B.new(1).x
class DD < Data; end
C = DD.define(:z); p C.superclass, C.new(z: 1)
V = Data.define(:uids, :earlier) do
  def initialize(uids:, earlier:) = super(uids: uids.to_s, earlier: !!earlier)
end
[->{ V["1234", earlier: true] }, ->{ V["1", true] }, ->{ V.new("1", earlier: true) }, ->{ V[uids: 1, earlier: nil] }, ->{ V["x"] }].each do |f|
  p f.call
rescue => e
  p [e.class, e.message]
end
P = Data.define(:a, :b)
[->{ P[1, b: 2] }, ->{ P.new(1, b: 2) }, ->{ P[1, {b: 2}] }].each { |f| p((f.call rescue [$!.class, $!.message])) }
[->{ P[1, 2, 3] }, ->{ P[1] }, ->{ P.new }, ->{ P[a: 1] }, ->{ P[a: 1, b: 2, c: 3] }].each { |f| p((f.call rescue [$!.class, $!.message])) }
W = Data.define(:a, :b) { def initialize(a:, b: 9) = super }
p W[1], W[1, 2], (W[1, 2, 3] rescue $!.message), (W[1, b: 2] rescue $!.message)
