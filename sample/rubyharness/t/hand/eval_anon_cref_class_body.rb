# a class body evaluated under an anonymous cref (class_eval / instance_eval on
# Module.new) keeps its constants in that class, even when the same source runs
# again for another module (racc's generated parsers)
SRC = <<'S'
class P < Object
  module_eval(<<'...end', 'x.y', 3)
    def helper = 1
...end
  Rd = false
  X = 1
  def chk = [self.class::Rd, X]
  module Inner; Y = 2; def self.y = Y; end
end
RES = P.new.chk
S
class Harness
  def run_class_eval = Module.new { class_eval(SRC) }
  def run_instance_eval = Module.new { instance_eval(SRC) }
end
h = Harness.new
m1 = h.run_class_eval
m2 = h.run_class_eval
p m1::P.constants.sort, m2::P.constants.sort, m1::P.equal?(m2::P), m2::RES, m2::P::Inner.y
m3 = h.run_instance_eval
k = m3.singleton_class.const_get(:P, false)
p k.constants.sort, k.new.chk
c1 = Module.new { instance_eval("class Q; X = 1; def x = X; end") }
c2 = Module.new { instance_eval("class Q; X = 1; def x = X; end") }
p c2.singleton_class::Q.constants, c2.singleton_class::Q.new.x
