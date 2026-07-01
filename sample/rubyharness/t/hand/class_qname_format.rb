# A class stringifies to its qualified name in every formatter path
# (interpolation, container inspect, puts). vs ruby.
module M
  class C; end
  module Inner; class E; end; end
end
p "#{M::C}"
p "cls #{M::Inner::E}"
p [M::C, M::Inner::E]
p [M::C].to_s
p({ cls: M::C })
p M::C.to_s
p "#{M::C.new.class}"
class Top; end
p "#{Top}"
p [Top, M::C, M::Inner::E].map(&:to_s)
p [M::C].inspect
