p Complex(4, 6).fdiv(2)
p Complex(1, 2).fdiv(4)
p (begin; Complex(1,2).fdiv("x"); rescue TypeError; "TE"; end)
