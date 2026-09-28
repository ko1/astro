# @@x in a block is lexical: Module.new do ... end inside SH writes SH's class variable; class_variables includes modules
module SH
  def get = @@cv
  def self.for(v)
    Module.new do
      include SH
      @@cv = v
    end
  end
end
m = SH.for(42)
p SH.class_variables, m.class_variables, Object.new.extend(m).get
p((Class.new { class_variable_set(:@@q, 2); def self.q = class_variable_get(:@@q) }).q)
class A; @@a = 1; end; module M; @@m = 2; end; class B < A; include M; @@b = 3; end; p B.class_variables, B.class_variables(false)
