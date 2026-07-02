# a define_method'd method enforces arity like a method (block, lambda, and
# plain-proc sources alike). vs ruby.
class C
  define_method(:req2) { |a, b| [a, b] }
  define_method(:zero) { || :z }
  define_method(:opt) { |a, b = 9| [a, b] }
  define_method(:rest) { |a, *r| [a, r] }
end
p C.new.req2(1, 2)
begin; C.new.req2(1); rescue ArgumentError; p :r1; end
begin; C.new.req2(1, 2, 3); rescue ArgumentError; p :r3; end
begin; C.new.zero(1); rescue ArgumentError; p :z1; end
p C.new.opt(1)
p C.new.opt(1, 2)
begin; C.new.opt(1, 2, 3); rescue ArgumentError; p :o3; end
p C.new.rest(1, 2, 3)
prc = proc { |a, b| [a, b] }
C.send(:define_method, :viaproc, prc)
begin; C.new.viaproc(1); rescue ArgumentError; p :vp; end
p C.new.viaproc(5, 6)
