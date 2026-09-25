# `Kernel#binding` reached by `super` has to answer the frame that called it.
# The global C builtins that ask who called them read the link cell the call
# site baked at args[-3]; `super` into a C global reserves no header cells, so
# there was nothing there and koruby raised "non-Ruby caller".  rubocop's
# HTMLFormatter::ERBContext redefines `binding` as `super` purely to make
# Kernel#binding public, so `--format html` could not render at all.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 55]]
end

class A
  def initialize = (@x = 1)
  def binding
    super
  end
end

class B
  def initialize = (@y = 2)
  def mybind = binding
end

module Mx; def binding = super; end
class D; include Mx; def initialize = (@z = 3); end

t(:super_binding)  { b = A.new.binding; [b.class, b.eval("@x"), b.receiver.class] }
t(:plain_binding)  { b = B.new.mybind;  [b.class, b.eval("@y"), b.receiver.class] }
t(:send_binding)   { A.new.send(:binding).class }
t(:method_obj)     { A.new.method(:binding).call.class }
t(:module_super)   { b = D.new.binding; [b.class, b.eval("@z")] }
t(:locals)         { a = A.new; def a.binding2 = binding; q = 1; a.binding.local_variables.class }
t(:erb)            { require "erb"; ERB.new("<%= @x %>").result(A.new.binding) }
t(:erb_plain)      { require "erb"; ERB.new("<%= @y %>").result(B.new.mybind) }
t(:twice)          { [A.new.binding.class, A.new.binding.class] }
t(:receiver)       { A.new.binding.receiver.instance_variable_get(:@x) }
t(:block_given)    { c = Class.new { def bg = super; def go = bg }; c.new.go rescue $!.class }
t(:src_loc)        { A.new.binding.source_location.first.end_with?("binding_via_super.rb") }
