p(FloatDomainError < RangeError)
def t; yield; rescue => e; e.class; end
p t { (1.0 / 0).to_i }
p t { Float::INFINITY.round }
p t { (0.0 / 0).to_i }
p t { Float::INFINITY.floor }
p t { Float::INFINITY.ceil }
p t { (-1.0 / 0).truncate }
p 3.7.to_i
p 3.7.round
p 3.2.floor
