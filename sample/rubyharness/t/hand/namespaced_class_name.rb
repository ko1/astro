# A class/module defined lexically inside a module carries the qualified name
# (M::C) in #name / #to_s / #inspect / error messages. vs ruby.
# (Object#inspect address+ivars are intentionally not compared -- moving GC has
# no stable address; only the qualified class name is checked.)
module M
  class C; end
  class D < C; end
  module Inner
    class E; end
    module Deeper; class F; end; end
  end
end
p M.name
p M::C.name
p M::C.to_s
p M::C.inspect
p M::D.name
p M::Inner.name
p M::Inner::E.name
p M::Inner::Deeper::F.name
p M::C.new.class.name
p M::Inner::E.new.class.to_s
class Top; end
p Top.name
module N; class G; end; end
p N::G.name
# anonymous
p Class.new.name
p Module.new.name
# reopening keeps the name
module M; class C; end; end
p M::C.name
# instance inspect contains the qualified name
p M::C.new.inspect.start_with?("#<M::C")
p [M::Inner::E.new].inspect.include?("M::Inner::E")
# error message uses the qualified name
begin; M::C.new.nope; rescue NoMethodError => e; p e.message; end
