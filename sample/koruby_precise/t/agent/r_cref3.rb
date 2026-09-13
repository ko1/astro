module Other
  class A
    def zzz; end
  end
end
class OuterY
  class A
    eval "class B; end"
    def c
      eval "class C; end"
    end
  end
end
OuterY::A.new.c
p OuterY::A.constants(false).sort
p Other::A.constants(false).sort
p Object.constants(false).include?(:C)
