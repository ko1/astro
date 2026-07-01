# defined?(assignment-expr) returns "assignment" without evaluating it. vs ruby.
p defined?(x = 5)
p defined?(@a = 1)
p defined?(@@cv = 2)
p defined?($g = 3)
p defined?(K = 4)
p defined?(h = {})
p defined?(x += 1)
p defined?(y ||= 5)
p defined?(@z &&= 5)
# non-assignments keep their classifications
p defined?(1 + 1)
p defined?("str")
p defined?([1, 2, 3])
p defined?(nil)
p defined?(String)
w = 9
p defined?(w)
