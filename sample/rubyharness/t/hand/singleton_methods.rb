module Mix; def mixed; end; end
class C; def regular; end; end
o = C.new
def o.sing1; end
def o.sing2; end
p o.singleton_methods.sort
p o.singleton_methods(false).sort
e = C.new
e.extend(Mix)
p e.singleton_methods.sort
p e.singleton_methods(false)
p C.new.singleton_methods
