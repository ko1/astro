p 2.4e20.round(-20), -2.4e20.round(-20), 2.5e200.round(-200) == 3 * 10**200, 2.4e200.round(-200) == 2 * 10**200
p 767573.1875850001.round(5), 767573.1875850001.round(5, half: :down), 767573.1875850001.round(5, half: :even), -767573.1875850001.round(5, half: :up)
p 2.5.round(half: :even), 0.5.round, 1.15.round(1), 1.005.round(2), 12345.6789.round(-2), 1e300.round(-299).class
p 1e-20.to_r, 5e-324.to_r.denominator.bit_length, 1e300.to_r.numerator.bit_length, 0.5.to_r, -0.75.to_r, 3.0.to_r, 1e20.to_r, 2.0**62, (2.0**62).to_r, (2.0**70).to_r, 0.1.to_r
p 1e15, 1e16, 1.5e15, 1234567890123456.7, 3453451587639734.5, 999999999999999.9, 1e14, 12345678901234567.0, 0.0001, 0.00001, 1.2345e-5, 1e22, 5e-324
p 4.723512644304606e+15.round(half: :down), 767573.1875850001.round(5, half: :down), 1.005.round(2), 2.675.round(2), 1e-20.round(20)
p Rational(10**400, 3 * 10**399).to_f, Rational(2**80 + 1, 2**80).to_f
