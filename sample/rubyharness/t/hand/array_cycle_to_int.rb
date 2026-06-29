r = []; [1, 2].cycle(2) { |x| r << x }; p r
class TI; def to_int; 2; end; end
r2 = []; [1, 2].cycle(TI.new) { |x| r2 << x }; p r2
def t; yield; rescue TypeError; "TE"; end
p t { [1, 2].cycle(Object.new) { } }
p [1, 2, 3].cycle(0) { |x| }
