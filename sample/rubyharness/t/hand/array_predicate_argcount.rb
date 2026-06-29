def t; yield; rescue ArgumentError; "AE"; end
p t { [1, 2, 3].one?(1, 2) }
p t { [1, 2, 3].all?(1, 2) }
p t { [1, 2, 3].any?(1, 2) }
p t { [1, 2, 3].none?(1, 2) }
p [1, 2, 3].all?(Integer)
p [1, 2, 3].one?(2)
p [1, 2, 3].any? { |x| x > 2 }
