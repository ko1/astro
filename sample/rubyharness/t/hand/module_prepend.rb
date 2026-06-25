# Module#prepend with super, ancestors, is_a?, multiple prepend (rubyspec follow-up)
module PA; def who; "A->" + super; end; end
module PB; def who; "B->" + super; end; end
class PBase; def who; "Base"; end; end
class PC < PBase
  prepend PA
  prepend PB
  def who; "C->" + super; end
end
p PC.new.who
p PC.ancestors
p PC.new.is_a?(PA)
p PC.new.is_a?(PB)

module PMod; def hi; "M"; end; end
class PD; end
PD.prepend(PMod)
p PD.new.hi

module PInc; def g; "I"; end; end
module PPre; def g; "P->" + super; end; end
class PE; include PInc; prepend PPre; end
p PE.new.g

# const_set / remove_method / undef_method / private_constant
class CM
  FOO = 1
  private_constant :FOO
  def foo_val; FOO; end
  def keep; "keep"; end
  def drop; "drop"; end
  remove_method :drop
end
p CM.new.foo_val
p CM.new.keep
CM.const_set(:BAR, 42)
p CM::BAR
p(CM.new.respond_to?(:drop))
