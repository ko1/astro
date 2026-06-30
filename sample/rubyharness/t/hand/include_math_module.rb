# include Math makes the module functions instance methods (module_function),
# while Math.<fn> keeps working. vs ruby.
class WithMath
  include Math
  def root(x); sqrt(x); end
  def ln(x); log(x); end
end
w = WithMath.new
p w.root(25.0)
p w.ln(Math::E).round(6)
p w.send(:sin, 0.0)
p w.send(:log2, 16.0)
p w.send(:atan2, 1.0, 1.0).round(6)
# module functions unaffected
p Math.sqrt(49)
p Math.cos(0.0)
p Math.hypot(3, 4)
