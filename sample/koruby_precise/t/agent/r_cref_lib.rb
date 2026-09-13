class OuterX
  class A
    eval "class B; end"
    def c
      eval "class C; end"
    end
  end
end
