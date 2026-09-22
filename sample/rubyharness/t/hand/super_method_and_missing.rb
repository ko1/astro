# Method#super_method across a prepend, `super` into a C builtin, and the
# NameError shape a bare-name miss keeps when a user #method_missing calls super.

# 1. `m = m.super_method` must WALK the MRO.  Resolving the super owner by name
#    from that class walks its own prepends and hands back the method we just
#    stepped off — dry-cli's .auto_initialize_keywords loops on that forever.
Pre = Module.new { def ai(from_prepend: nil, **kw) = super(**kw) }
Inc = Module.new { def ai(from_include: nil, **kw) = super(**kw) }
class Base; def ai(**kw) = nil; end
K = Class.new(Base) do
  prepend Pre
  include Inc
  def ai(own: nil, **kw) = super(**kw)
end

m = K.instance_method(:ai)
n = 0
while m && n < 12
  p [n, m.owner, m.parameters.map(&:first)]
  m = m.super_method
  n += 1
end
p [:iters, n]

# the bound form walks the same chain
bm = K.new.method(:ai)
owners = []
while bm
  owners << bm.owner
  bm = bm.super_method
end
p owners

# 2. `super` into a global C function (Kernel#abort): korb_super used to hand the
#    variadic builtin to the ISEQ path, which read its NULL opt_defaults.
module AbortHook
  def abort(*args) = super
end
class Aborter
  include AbortHook
  def go = abort            # no message: a stderr write would race the stdout diff
end
begin
  Aborter.new.go
rescue SystemExit => e
  p [:exit, e.status]
end

# 3. A bare name (`zork`) that a user #method_missing hands on with `super`
#    raises NameError, not NoMethodError (rspec-core checks the class).
class MM
  def method_missing(name, *args) = super
  def respond_to_missing?(n, p = false) = false
  def bare = zork
  def called = zork()
  def with_recv = self.zork
end
[:bare, :called, :with_recv].each do |meth|
  begin
    MM.new.__send__(meth)
  rescue NoMethodError, NameError => e
    p [meth, e.class, e.message]
  end
end
