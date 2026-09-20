# A class whose ancestry skips Object gets neither Kernel's global functions nor
# the top-level constant fallback.  builder's XmlBase (< BasicObject) is built on
# both: `p`/`raise` become XML tags through #method_missing, and a bare
# `Exception` is a NameError it catches to pick its escaping fallback.
class B < BasicObject
  def seen; @seen; end
  def method_missing(n, *a); @seen = [n, a.size]; :mm; end
  def call_raise; [(raise ::ArgumentError, "x"), @seen]; end
  def call_puts;  [puts("hi"), @seen]; end
  def call_p;     [p(1), @seen]; end
  def bare_const;   Exception rescue "ERR #{$!.class}"; end
  def bare_const2;  Array rescue "ERR #{$!.class}"; end
  def abs_const;    ::Exception; end
end
b = B.new
p b.call_raise
p b.call_puts
p b.call_p
p b.bare_const
p b.bare_const2
p b.abs_const

# a module body still falls back to Object (CRuby retries the search there)
module M
  Y = 2
  def self.look; [Exception, Array, Y]; end
  class Inner < BasicObject
    def look; [(Y rescue "ERR"), (::Exception rescue "ERR")]; end
  end
end
p M.look
p M::Inner.new.look

# an ordinary class is unaffected
class D
  def look; [Exception, Array]; end
  def call_p; p 7; end
end
p D.new.look
D.new.call_p

# ... forwarding: a bare `super` forwards everything the caller passed
class A
  def initialize(x, y); @x, @y = x, y; end
  def show; [@x, @y]; end
  def m(a, b = 2, *r, k: 9, **kw, &blk); [a, b, r, k, kw, blk&.call]; end
end
class C < A
  def initialize(...)
    @pre = 1
    super
  end
  def m(...)
    super
  end
end
p C.new(1, 2).show
p C.new(1, 2).m(5)
p C.new(1, 2).m(5, 6, 7, k: 1, z: 2) { :blk }

# Time.new keeps a fractional seconds argument when a zone is given too
t = Time.new(2018, 1, 14, 22, 25, 19.1r, "+09:00")
p [t.sec, t.subsec, t.utc_offset]
p Time.new(2018, 1, 14, 22, 25, 19.5, "+09:00").subsec
p Time.new(2018, 1, 14, 22, 25, 19.5).subsec
