def t; yield; rescue => e; e.class; end
p t { 5 => Integer }            # matches → nil, no error
p t { 5 => 1 }                  # NoMatchingPatternError
p t { case 5; in 1; :one; in 2; :two; end }   # NoMatchingPatternError
r = case [1, 2]; in [a, b]; a + b; end
p r                             # 3
p(NoMatchingPatternError < StandardError)
p(NoMatchingPatternKeyError < NoMatchingPatternError)
