# A value with no singleton class — Integer/Symbol/Float, and an interned (fstring)
# String: a frozen literal, `"lit".freeze`, `-str` — raises TypeError on a `def`
# through instance_eval(String) as well, instead of leaking a global function;
# non-def code in the string still runs.  (pry's binding_for falls back on it.)
[:sym, 1, 1.5, 10**30, "lit".freeze, -"dyn#{1}", nil, true].each { |v|
  r = begin; v.instance_eval("def foo_ie; :x; end"); :ok; rescue => e; [e.class, e.message]; end
  r2 = v.instance_eval("self.class")
  r3 = begin; v.instance_eval { def foo_b; end }; :ok; rescue => e; [e.class, e.message]; end
  r4 = begin; v.singleton_class; :sclass; rescue TypeError; :type_error; end
  p [v, r, r2, r3, r4]
}
p((foo_ie rescue $!.class), nil.foo_ie)
x = 1; p((def x.m; end rescue [$!.class, $!.message]))
w = "lit".freeze; p((def w.m; end rescue [$!.class, $!.message]))
z = String.new("q").freeze; p((def z.m; end rescue $!.class), z.singleton_class.is_a?(Class))
