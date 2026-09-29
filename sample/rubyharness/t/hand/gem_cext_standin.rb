# Activating a gem that ships a C extension koruby did not build (bigdecimal: its
# lib/bigdecimal.rb requires bigdecimal.so, then redefines ** with C-only helpers)
# uses koruby's own bigdecimal for the whole feature, as a plain require does.
gem "bigdecimal"
require "bigdecimal"
p BigDecimal("1.1") * 2, (BigDecimal("2") ** 10).to_i, BigDecimal("0.1") + BigDecimal("0.2") == BigDecimal("0.3")
