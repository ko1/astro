# Symbol#to_proc returns a lambda (CRuby); the call still dispatches arg.sym(rest).
# Method#to_proc stays a bound-receiver lambda. vs ruby.
p :upcase.to_proc.lambda?
p :upcase.to_proc.call("hi")
p :to_s.to_proc.call(42)
p [1, 2, 3].map(&:to_s)
p ["a", "b"].map(&:upcase)
p [1, -2, 3].select(&:positive?)
m = "hello".method(:upcase)
p m.to_proc.call
p m.to_proc.lambda?
p [1, 2, 3].map(&5.method(:+))
p [1, 2, 3].each_with_object("") { |x, s| s << x.to_s }
