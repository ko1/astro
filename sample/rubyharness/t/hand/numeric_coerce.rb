class MyNum < Numeric
  def initialize(v); @v = v; end
  def to_f; @v.to_f; end
end
a = MyNum.new(3)
p a.coerce(MyNum.new(5)).map { |x| x.class }
p a.coerce(2.5)
p a.coerce(10)
def t; yield; rescue TypeError; "TE"; end
p t { a.coerce(nil) }
p t { a.coerce(true) }
p t { a.coerce(Object.new) }
p 1.coerce(2.5)
p 2.5.coerce(1)
p 5.coerce(3)
