# Enumerator::Lazy exists and is the class of lazy-mode enumerators. vs ruby.
p Enumerator::Lazy
p Enumerator::Lazy.superclass
p (Enumerator::Lazy < Enumerator)
p [1, 2, 3].lazy.class
p [1, 2, 3].lazy.map { |x| x * 2 }.class
p [1, 2, 3].lazy.select { |x| x > 1 }.class
p [1, 2, 3].lazy.is_a?(Enumerator::Lazy)
p [1, 2, 3].lazy.is_a?(Enumerator)
# plain (non-lazy) enumerators are still Enumerator
p [1, 2, 3].each.class
p [1, 2, 3].to_enum.class
# lazy chain still computes correctly and stays bounded
p (1..Float::INFINITY).lazy.select { |x| x.even? }.first(3)
