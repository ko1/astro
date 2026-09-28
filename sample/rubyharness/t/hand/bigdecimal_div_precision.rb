require "bigdecimal"
[["1", 3], ["2", 3], ["1", 7], ["123456789012345678901234567890", 7], ["1.5", "3.3"], ["1", "3.000000000000000000000000000000000000001"], ["10", 3], ["0.001", 3]].each do |a, b|
  r = BigDecimal(a) / (b.is_a?(String) ? BigDecimal(b) : b)
  puts "#{a}/#{b} => #{r} (#{r.precision})"
end
p %w[1e10 0.001 123.45 100 0 0.1 -0.0012 1.5e-10].map { BigDecimal(_1).precision }
BigDecimal.mode(BigDecimal::ROUND_MODE, :half_even)
p BigDecimal("1") / BigDecimal("8"), BigDecimal("2") / 3, (BigDecimal("1") / 3) * 3
BigDecimal.mode(BigDecimal::ROUND_MODE, :half_up)
old = BigDecimal.limit(10); p BigDecimal("1") / 3; BigDecimal.limit(old)
p BigDecimal("1").div(3, 5), BigDecimal("1").quo(3), BigDecimal("12345678901234567890123456789012345678901234567890") / 7
