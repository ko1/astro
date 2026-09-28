# BigDecimal._dump / _load use "<words*9>:<value>" (Psych !ruby/object:BigDecimal)
require "bigdecimal"; require "yaml"
d = BigDecimal("0.9"); p d._dump, BigDecimal._load(d._dump), BigDecimal._load("18:0.9e0")
p YAML.unsafe_load("--- !ruby/object:BigDecimal 81:0.31415926535897932384626433832795028841971693993751058209749445923e1\n")
p((BigDecimal._load("0.9e0") rescue [$!.class, $!.message]))
p Marshal.load(Marshal.dump(BigDecimal("12.5")))
require "bigdecimal"
%w[0 0.9 12.5 1 123456789 1234567890 0.001 0.0000000001 1e20 -3.25 NaN Infinity -Infinity 1e-20 123456789.123456789 1000000000].each { |s| puts "#{s} #{BigDecimal(s)._dump}" }
