# define_method coerces the name via #to_str and raises FrozenError on a frozen
# class. vs ruby.
class C; end
class Nm; def to_str; "dyn"; end; end
C.define_method(Nm.new) { 7 }
p C.new.dyn
class Bad; def to_str; 99; end; end
begin; C.define_method(Bad.new){}; rescue TypeError; p :type_err; end
C.define_method(:s) { 1 }; C.define_method("t") { 2 }
p [C.new.s, C.new.t]
D = Class.new.freeze
begin; D.define_method(:z){}; rescue => e; p e.class; end
