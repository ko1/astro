class O; def ==(o); o == 42; end; end
o = O.new
p(42 == o)
p(5 == o)
p(42 != o)
p(5 != o)
p(3.14 == o)
p("x" == o)
p(1 == 1)
p(1 == 2)
