class Outer
  class A
    eval "class B; end"
    def c
      eval "class C; end"
    end
  end
end
Outer::A.new.c
p(defined?(Outer::A::B))
p(defined?(Outer::A::C))
p(Outer::A.constants(false).sort)
