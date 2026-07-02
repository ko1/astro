# a bare constant read inside a nested class resolves the lexically-innermost
# constant even when a same-named class exists in an outer namespace. vs ruby.
module M
  class A; def who; "outer_A"; end; end
  class B < A; end
  class C < B; end          # outer M::C
  module Inner
    module A                 # inner M::Inner::A (shadows M::A lexically)
      def who; "inner_A"; end
    end
    class C                  # same name as M::C
      include A             # must be M::Inner::A
    end
  end
end
p M::Inner::C.ancestors.map(&:to_s)
p M::Inner::C.new.who
p M::C.ancestors.map(&:to_s)
