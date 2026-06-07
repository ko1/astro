# L0: symbols
p :hello
p :hello.to_s
p "world".to_sym
p :abc == :abc
p :abc == :abd
p :hello.length
p :hello.size
p :hello.upcase
p :upcase.to_proc.call("bar")
p [:a, :b, :c].map(&:to_s)
p :abc <=> :abd
p :z.succ
p "a_b".to_sym
p :"dynamic#{1 + 1}"
p %i[a b c]
