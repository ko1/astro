# Array#sum uses Kahan-Babuska-Neumaier compensated summation (precise floats),
# preserving Infinity/NaN. vs ruby.
p [1.0, 2.0, 3.0].sum
p ([0.1] * 10).sum
p ([0.1] * 100).sum
p [Float::MAX, 1.0, -Float::MAX].sum
p [Float::INFINITY, 1.0].sum
p [Float::INFINITY, -Float::INFINITY].sum
p [1.0, Float::NAN].sum.nan?
p [1, 2.0, 3].sum
p [1, 2, 3].sum
p [1.5, 2.5].sum
