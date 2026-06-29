p "a".upto("e").to_a
class TS; def to_str; "c"; end; end
p "a".upto(TS.new).to_a
out = []; "1".upto("5") { |x| out << x }; p out
def t; yield; rescue TypeError; "TE"; end
p t { "a".upto(5).to_a }
