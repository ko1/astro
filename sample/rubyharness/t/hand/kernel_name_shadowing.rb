# A Kernel name the parser LOWERS to a node is still an ordinary method: a class
# (or a `let`) that defines its own wins.  koruby guarded `binding` but not
# `proc` / `lambda`, so rspec's `let(:proc) { -> v { } }` never reached the
# user's method — transproc's spec does exactly that.
# `Module#refine` / `#using` are private in CRuby, so `mod.methods` must not
# list them (transproc asserts that on a Registry singleton).
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

class S
  def binding = :my_binding
  def proc(&b) = :my_proc
  def lambda(&b) = :my_lambda
  def raise(*) = :my_raise
  def loop = :my_loop
  def format(*) = :my_format
  def go   = [binding, proc {}, lambda {}, raise, loop, format]
  def noblk = [proc, lambda]
end

t(:shadowed)    { S.new.go }
t(:no_block)    { S.new.noblk }
t(:arrow_stays) { S.new.instance_eval { (-> { :arrow }).call } }
t(:real_proc)   { Class.new { def go = proc { :p }.call }.new.go }
t(:real_lambda) { Class.new { def go = lambda { :l }.lambda? }.new.go }
t(:real_arrow)  { (-> (x) { x * 2 }).call(3) }

module Mp; def proc(&b) = :mod_proc; end
class S2; include Mp; def go = proc {} ; end
t(:module_shadow) { S2.new.go }

t(:refine_priv)  { Module.private_instance_methods(false).include?(:refine) }
t(:using_priv)   { Module.private_instance_methods(false).include?(:using) }
t(:refine_pub)   { Module.public_instance_methods(false).include?(:refine) }
t(:mod_methods)  { Module.new.methods.include?(:refine) }
t(:call_refine)  { Module.new.refine(String) { } }
t(:refine_works) { m = Module.new { refine(String) { def shout = upcase } }; m.class }
t(:cls_refine)   { Class.new.methods.include?(:refine) }
