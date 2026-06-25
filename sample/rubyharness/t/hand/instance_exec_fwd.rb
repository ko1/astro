# instance_exec/instance_eval with a forwarded closure Proc (rubyspec follow-up; was a SEGV)
x = 10
pr = proc { |a| @v = a + x; @v }
o = Object.new
p o.instance_exec(5, &pr)
p o.instance_variable_get(:@v)

pr2 = proc { @w = "self=" + self.class.to_s }
p o.instance_eval(&pr2)

# curry + instance_exec (the original crash): a curried Proc passed as &block
padd = proc { |a, b, c| (a || 0) + (b || 0) + (c || 0) }
curried = padd.curry.call(1, 2)
p curried.call(3)
p instance_exec(3, &curried)

# define_method / define_singleton_method with a forwarded Proc
pr3 = proc { |x| x * 3 }
ob = Object.new
ob.define_singleton_method(:triple, &pr3)
p ob.triple(5)
class DMFwd
  define_method(:dbl, &proc { |x| x * 2 })
end
p DMFwd.new.dbl(7)
