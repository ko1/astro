# include pulls in a module's own included modules transitively, with correct
# ancestor order; cyclic include and no-arg raise; re-include is deduped. vs ruby.
module MZ; def mz; "mz"; end; ZC = "z"; end
module MA; include MZ; def ma; "ma"; end; end
module MB; def mb; "mb"; end; end
class C; include MA; include MB; end
p C.ancestors.map(&:to_s)
p [C.new.ma, C.new.mb, C.new.mz]
p C.include?(MZ)
p C.new.class::ZC rescue p $!
# subclass already including via super
class D < C; include MA; end
p D.ancestors.map(&:to_s)
# cyclic
module X; end; module Y; include X; end
begin; X.include(Y); rescue ArgumentError; p :cyclic; end
begin; Class.new { include }; rescue ArgumentError; p :noarg; end
