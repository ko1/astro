def t; yield; rescue Math::DomainError => e; "DE"; end
p t { Math.sqrt(-1) }
p t { Math.asin(2) }
p t { Math.acos(2) }
p t { Math.acosh(0.5) }
p Math.sqrt(4)
p Math.sin(Float::INFINITY)
p Math.sqrt(Float::NAN).nan?
p (Math::DomainError < StandardError)
def t; yield; rescue Math::DomainError; "DE"; end
p t { Math.log(-1) }
p t { Math.log10(-5) }
p t { Math.log2(-2) }
p Math.log(0)
p Math.log(Math::E).round(5)
p Math.log10(100)
p Math.log(8, 2)
p Math.sqrt(-1) rescue (p "sqrt-de")
