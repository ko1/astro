# Float#<=> and the comparison operators are unordered for NaN. vs ruby.
nan = 0.0 / 0.0
p(nan <=> 1.0)
p(1.0 <=> nan)
p(nan <=> nan)
p(nan < 1.0)
p(nan > 1.0)
p(nan == nan)
p(nan <= 1.0)
p(2.0 <=> 2.0)
p(1.0 <=> 2.0)
p((1.0/0.0) <=> 2.0)
