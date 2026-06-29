p FrozenError.new.message
p KeyError.new.message
p StopIteration.new.message
p FloatDomainError.new.message
p NoMatchingPatternError.new.message
def t; yield; rescue => e; e.class.to_s; end
p t { [1].freeze << 2 }
p t { {}.fetch(:missing) }
