# prepend pulls in a module's own included modules; ancestor order + super. vs ruby.
module MA; def ma; "ma"; end; end
module MB; include MA; def mb; "mb"; end; end
class C
  prepend MB
  def mb; "C-" + super; end
  def own; "own"; end
end
p C.new.mb
p C.new.ma
p C.new.own
p C.ancestors.map(&:to_s)
p C.instance_methods(false).sort
# cyclic prepend
module X; end; module Y; include X; end
begin; X.prepend(Y); rescue ArgumentError; p :cyclic; end
