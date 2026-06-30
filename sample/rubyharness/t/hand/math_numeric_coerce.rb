# Math functions coerce a Numeric (subclass) argument via #to_f, but reject a
# plain object that merely defines #to_f (CRuby rb_to_float). vs ruby.
class NumF < Numeric
  def initialize(f); @f = f; end
  def to_f; @f; end
end
class PlainF
  def to_f; 8.0; end
end
p Math.sqrt(NumF.new(16.0))
p Math.log2(NumF.new(8.0))
p Math.log10(NumF.new(1000.0))
p Math.log(NumF.new(1.0))
p Math.sin(NumF.new(0.0))
p Math.atan2(NumF.new(0.0), NumF.new(1.0))
p Math.hypot(NumF.new(3.0), NumF.new(4.0))
begin; Math.sqrt(PlainF.new); rescue => e; p e.class; end
begin; Math.log2(Object.new); rescue => e; p e.class; end
# ordinary numeric args still work
p Math.sqrt(2)
p Math.log2(2**64)
