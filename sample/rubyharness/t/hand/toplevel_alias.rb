def original; "hi"; end
alias aliased original
p aliased
def greet(n); "Hi #{n}"; end
alias hello greet
p hello("Bob")
class C; def m; "c"; end; alias m2 m; end
p C.new.m2
p C.new.m
module M; def x; "mx"; end; alias y x; end
class D; include M; end
p D.new.y
def base_method; __method__; end
alias bm base_method
p bm
