# Namespaced constants coexist and resolve lexically: M::C and a top-level C are
# distinct, M::X scoped read finds M's X, and a bare read in a nested scope walks
# the enclosing chain (own -> outer module -> top-level -> builtin). vs ruby.
STATUS = :top
module A
  STATUS = :a
  X = 1
  module B
    STATUS = :b
    Y = 2
    def self.check; [STATUS, X, Y]; end
    class C
      Z = 3
      def get; [STATUS, X, Y, Z]; end
    end
  end
end
p STATUS
p [A::STATUS, A::B::STATUS]
p [A::X, A::B::Y, A::B::C::Z]
p A::B.check
p A::B::C.new.get
# builtin module consts (owner nil) still resolve via fallback
p Math::PI.round(5)
p Float::INFINITY
# colliding class names in different namespaces
class Task; def kind; :top; end; end
module Sched; class Task; def kind; :sched; end; end; end
p [Task.new.kind, Sched::Task.new.kind]
p [Task.name, Sched::Task.name]
module Sched; class Task; def extra; :ex; end; end; end
p [Sched::Task.new.kind, Sched::Task.new.extra, Task.new.respond_to?(:extra)]
# error messages
begin; A::NOSUCH; rescue NameError => e; p e.message; end
begin; NOSUCH_TOP; rescue NameError => e; p e.message; end
