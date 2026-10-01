# A def on a plain object's singleton class skips the global cache flush when no
# cache resolved that name from it.  Every cached path must still see the new method.
class C
  def foo = :c_foo
  def bar = :c_bar
  def call_foo = foo
  def method_missing(n, *a) = n == :ghost ? :mm_ghost : super
  def respond_to_missing?(n, p = false) = n == :ghost || super
end

def site(o) = o.foo
def site_bar(o) = o.bar
def site_ghost(o) = o.ghost

# 1. cached on C, then a singleton def (creates the singleton)
o = C.new
3.times { site(o) }
def o.foo = :o_foo
p site(o), o.call_foo

# 2. singleton exists and foo was resolved from it, then redefined there
3.times { site(o) }
def o.foo = :o_foo2
p site(o), o.call_foo

# 3. bar never looked up from the singleton, then defined (the skipped path)
def o.bar = :o_bar
p site_bar(o)

# 4. method_missing / respond_to? before and after a def of the same name
p site_ghost(o), o.respond_to?(:ghost), o.respond_to?(:baz)
def o.ghost = :o_ghost
def o.baz = :o_baz
p site_ghost(o), o.respond_to?(:baz)

# 5. super from a singleton method, and redefining the class method under it
def o.qux = [:o_qux, super]
class C; def qux = :c_qux; end
p o.qux
class C; def qux = :c_qux2; end
p o.qux

# 6. define_method / attr_accessor on the singleton, then use them
o.singleton_class.define_method(:dm) { :o_dm }
o.singleton_class.attr_accessor :acc
o.acc = 5
p o.dm, o.acc

# 7. htmlentities' shape: instance_eval defs per instance, interleaved calls
class Enc
  def initialize(tag)
    instance_eval "def enc_basic(x) = [:basic, #{tag.inspect}, x]"
    build_ext(tag)
  end
  def build_ext(tag) = instance_eval("def enc_ext(x) = [:ext, #{tag.inspect}, x]")
  def run(x) = [enc_basic(x), enc_ext(x)]
end
encs = 5.times.map { |i| Enc.new("t#{i}") }
encs.each { |e| p e.run(1) }
encs.each { |e| p e.run(2) }
e0 = encs[0]
e0.instance_eval "def enc_basic(x) = :redefined"
p e0.run(3), encs[1].run(3)

# 8. other instances are untouched
p C.new.foo, C.new.bar, site(C.new)
