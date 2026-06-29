p [1, 2].zip([3, 4], [5, 6])
p [1, 2, 3].zip([4, 5])
class TA; def to_ary; [10, 20, 30]; end; end
p [1, 2].zip(TA.new)
class TE; include Enumerable; def each; yield 7; yield 8; end; end
p [1, 2].zip(TE.new)
p [1, 2].zip(1..2)
out = []; [1, 2].zip([3, 4]) { |x| out << x }; p out
