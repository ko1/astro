# Top-level def called from main with a block (cached after the first call): break / next,
# Kernel#loop / #tap, a top-level tap and a singleton override behave as CRuby.  Run under both and diff.
def fb = yield + 1
p fb { 1 }, (fb { break 7 }), [1, 2].map { fb { _1 } }
r = loop { break :lp }; p r, tap { }.class
def tap = :mine
p tap { }
p fb { next 5 }
x = 0; 3.times { x += fb { 10 } }; p x
def self.fb = :sing
p fb { 1 }
