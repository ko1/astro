Point = Struct.new(:x, :y)
case Point.new(1, 2); in [x, y]; p [:arr, x, y]; end
case Point.new(1, 2); in [Integer => x, Integer => y]; p [:typed, x, y]; end
case Point.new(1, 2); in [1, b]; p [:lit, b]; end
class Tup; def deconstruct; [10, 20, 30]; end; end
case Tup.new; in [a, b, c]; p [:tup, a, b, c]; end
case Tup.new; in [a, *rest]; p [:splat, a, rest]; end
case Tup.new; in [*pre, last]; p [:pre, pre, last]; end
case Tup.new; in [first, *mid, last]; p [:mid, first, mid, last]; end
Coord = Data.define(:lat, :lng)
case Coord.new(1.0, 2.0); in [lat, lng]; p [:data, lat, lng]; end
case [1, 2, 3]; in [a, *rest]; p [:plain_splat, a, rest]; end
case Point.new(1, 2); in [a, b, c]; p :three; else; p :len_mismatch; end
p([1, 2] => [a, b]) rescue nil
case {name: "x"}; in {name:}; p name; end
