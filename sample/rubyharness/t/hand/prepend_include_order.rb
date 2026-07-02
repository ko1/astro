# include/prepend with multiple modules in one call → first arg nearest;
# instance_methods walks prepended/included modules; no-arg raises. vs ruby.
module M1; def m; "m1"; end; def only1; end; end
module M2; def m; "m2"; end; def only2; end; end
class C; prepend M1, M2; def m; "c"; end; def own; end; end
p C.ancestors.map(&:to_s)
p C.new.m
p C.instance_methods(false).sort
p C.instance_methods.include?(:only1)
p C.instance_methods.include?(:only2)
class D; include M1, M2; end
p D.ancestors.map(&:to_s)
p D.new.m
p D.instance_methods.include?(:only1)
begin; Class.new { prepend }; rescue ArgumentError; p :noarg_p; end
begin; Class.new { include }; rescue ArgumentError; p :noarg_i; end
