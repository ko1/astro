class Foo; end
Foo.class_eval { def bar; 42; end }
p Foo.new.bar
Foo.class_eval { def self.cm; :classmethod; end }
p Foo.cm
Foo.class_exec(10) { |n| define_method(:tens) { n } }
p Foo.new.tens
module M; end
M.module_eval { def mixed; :mixed; end }
class UsesM; include M; end
p UsesM.new.mixed
p(Foo.class_eval { 1 + 1 })
Foo.class_eval do
  attr_accessor :name
  def greet; "hi #{@name}"; end
end
f = Foo.new; f.name = "x"
p f.greet
p Foo.class_eval { self }
DSL = Class.new
DSL.class_eval { def configure; "configured"; end }
p DSL.new.configure
Bar = Class.new do
  def initialize(v); @v = v; end
  def v; @v; end
end
Bar.class_eval { def doubled; @v * 2; end }
p Bar.new(21).doubled
counter = 0
Foo.class_exec { counter += 1 }
p counter
