# Array#sum / Range#sum with a block use the compensated Float sum once the values are
# Float (CRuby does; hexapdf line widths came out 49.99 instead of 50.0).  Run under CRuby and koruby and diff.
p ([0.1]*10).sum, ([0.1]*10).sum(0.0), (1..10).sum { 0.1 }, ([0.1]*10).each.sum, [3, 0.1, 0.2, 0.3].sum
p [1e100, 1.0, -1e100].sum, [0.1, 0.2, 0.3].sum, {a: 0.1, b: 0.2, c: 0.3}.sum { _2 }
p [0.1r, 0.2].sum, [1, 2**70, 0.5].sum, ([0.1]*3).sum(1i)
p ([0.1]*10).inject(:+), ([0.1]*10).map { _1 }.sum, [0.1, Float::INFINITY, 1.0].sum, [Float::INFINITY, -Float::INFINITY].sum
p [0.1, 0.2, 0.3].each_slice(1).sum { _1[0] }, ([0.1] * 10).lazy.map { _1 }.sum
p [0.1, 0.2, 0.3].sum { _1 }, [1, 2].sum { _1 * 0.1 }, [0.1, "x"].sum("") { _1.to_s }, [1, 0.5, 2r].sum { _1 }, [0.1]*3 .then { 0 }
p ([0.1]*10).sum { _1 }, ([0.1]*10).sum(0.0) { _1 }, [1.0, 1e100, 1.0, -1e100].sum { _1 }, [0.1, 1i].sum { _1 }
p [Float::INFINITY, 1.0].sum { _1 }, [Float::NAN, 1.0].sum { _1 }.nan?, [2**70, 0.5].sum { _1 }
