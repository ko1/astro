# bigdecimal/util — the to_d conversions live in bigdecimal.rb.
require 'bigdecimal'

class BigDecimal
  # The plain decimal form ("3.14"), unlike #to_s's "0.314e1".
  def to_digits
    return to_s if nan? || infinite? || zero?
    i = to_i.to_s
    _, f, _, z = frac.split
    i + "." + ("0" * (-z)) + f
  end
end
