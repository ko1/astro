p 12345.digits
def t; yield; rescue Math::DomainError; "MDE"; end
p t { -5.digits }
p 255.digits(16)
p 0.digits
