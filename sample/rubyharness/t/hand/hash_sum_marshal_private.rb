# Hash#sum with a block: Float / Rational / Array / Bignum values; Marshal calls a non-public marshal_dump / _dump
h = {a: 1, b: 2}
p h.sum { |k, v| v * 0.5 }, h.sum { |k, v| v }, h.sum(0.0) { |k, v| v }, h.sum([]) { |k, v| [k] }, h.sum { |k, v| Rational(v, 3) }
p [{}.sum { 1 }, [1, 2].sum { _1 * 1.5 }, (1..3).sum { _1 / 2r }]
p({a: 0.1, b: 0.2, c: 0.3}.sum { _2 }, {a: 1, b: 2**62, c: 2**62}.sum { _2 }, {a: 1, b: 0.5}.sum(10) { _2 }, (1..10).to_h { [_1, 0.1] }.sum { _2 })
class P1; def initialize = (@a = 1; @f = -> {}); protected def marshal_dump = [@a]; private def marshal_load(x) = (@a = x[0]); end
x = Marshal.load(Marshal.dump(P1.new)); p x.instance_variable_get(:@a)
class P2; def initialize = (@a = 2); private def _dump(l) = @a.to_s; def self._load(s) = new; end
p Marshal.dump(P2.new).size
