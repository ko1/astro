# A bare `private` in a class BODY dies with the body: CRuby keeps the default
# on the cref, koruby on the class, so every body form has to clear it.  A later
# define_method/attr on that class is public (rspec-mocks' any_instance stubs a
# class built as `Class.new do ...; private; ... end` and the stub must stay
# public).
def probe(k, tag)
  k.__send__(:define_method, :zz) { :zz }
  k.__send__(:attr_accessor, :yy)
  p [tag, k.public_instance_methods(false).sort]
end
probe(Class.new { private; def a; end }, :class_new)
probe(Module.new { private; def a; end }, :module_new)
probe(Struct.new(:x) { private; def a; end }, :struct_new)
probe(Data.define(:y) { private; def a; end }, :data_define)
k = Class.new; k.class_eval { private; def a; end }; probe(k, :class_eval)
k = Class.new; k.class_exec { private; def a; end }; probe(k, :class_exec)
k = Class.new; k.module_eval("private; def a; end"); probe(k, :module_eval_str)
class K4; private; def a; end; end
probe(K4, :reopen)

# the body's own defs still honor it
p Class.new { private; def a; end }.private_instance_methods(false)
p Module.new { private; def a; end }.private_instance_methods(false)

# a real any_instance shape: back the method up, then redefine it
klass = Class.new do
  def existing_method; :orig; end
private
  def private_method; :pm; end
end
klass.class_exec { alias_method :backup, :existing_method }
klass.__send__(:define_method, :existing_method) { :proxied }
p [klass.public_instance_methods(false).sort, klass.private_instance_methods(false).sort]
p Class.new(klass).new.existing_method

# a Module subclass gets its #initialize from the MRO, not just its own table
module Init
  def initialize(tag, *rest)
    @tag = tag
    super(*rest)
  end
  def tag = @tag
end
class MyMod < Module
  include Init
end
m = MyMod.new(:hello)
p [m.tag, m.class, m.is_a?(Module)]
begin
  eval("class MyCls < Class; end")
rescue TypeError => e
  p e.message
end
