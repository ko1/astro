%w[1e2/3 1/3e2 1.5e-3 .5 5. 1_000.0_1 1__0 -3/-4 1e 1e+ 1E2 -1.2e1/2 1/2/3 0x10 1_e2 1e2_0 1e_2 1/2.5 +.5 -.5e1 123456789012345678901234567890 1.23456789012345678901234567890 12345678901234567890123/7 1e30 0.000000000000000000000001 abc 12abc 3/ /3 1.e2 1e2.5 00012 -0 1e-30].each do |s|
  r = (s.to_r rescue $!.class)
  puts "#{s.inspect} => #{r.inspect}"
end
p(("3/0".to_r rescue $!.class), "9.99e99".to_r == Rational(999, 100) * 10**99)
p "20010db80000000000080800200c417a".hex, "ffffffffffffffffffff".hex, "-ffffffffffffffffffff".hex, "0x1ffffffffffffffff".hex, "777777777777777777777777".oct, "3fffffffffffffff".hex, "-4000000000000000".hex, "7fffffffffffffff".hex, "-8000000000000000".hex, "  12_34".hex, "1__2".hex
p "123456789012345678901234567890".to_c, "-123456789012345678901234567890+2i".to_c, "1/3+12345678901234567890123/7i".to_c, ("3/0".to_c rescue $!.class), ("1+3/0i".to_c rescue $!.class), "1_000+2_0i".to_c
