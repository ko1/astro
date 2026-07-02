# include/prepend require a Module; a Class argument raises TypeError. vs ruby.
module M; def m; :m; end; end
class Host; include M; end
p Host.new.m
class Host2; prepend M; end
p Host2.new.m
class Cmp
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
p(Cmp.new(1) < Cmp.new(2))
begin; Class.new { include String }; rescue TypeError; p :inc_class_typeerror; end
begin; Class.new { prepend Array }; rescue TypeError; p :prep_class_typeerror; end
