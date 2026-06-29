p 5.coerce(2)
p 5.coerce(2.5)
p 5.coerce("2.5")
class TF; def to_f; 3.5; end; end
p 5.coerce(TF.new)
def t; yield; rescue ArgumentError; "AE"; rescue TypeError; "TE"; end
p t { 5.coerce("abc") }
p 10.coerce(1000000000000000000000)
