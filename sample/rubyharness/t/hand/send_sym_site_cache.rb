# o.send(:name, ...) remembers (class, name) at the site: a different name, class,
# redefinition, visibility change or undef through the same site must all be seen
class A
  def m(x) = [:a_m, x]
  def n(x) = [:a_n, x]
  def opt(x, y = 2) = [:a_opt, x, y]
  private def priv(x) = [:a_priv, x]
end
class B < A; def m(x) = [:b_m, x]; end
def via_send(o, name, x) = o.send(name, x)
def via_public_send(o, name, x) = o.public_send(name, x)
objs = [A.new, B.new]
3.times do |i|
  objs.each { |o| p via_send(o, :m, i), via_send(o, :n, i), via_send(o, :opt, i), via_send(o, :priv, i) }
end
# public_send refuses a private method, then sees it made public
begin
  via_public_send(A.new, :priv, 1)
rescue NoMethodError => e
  p e.message
end
class A; public :priv; end
p via_public_send(A.new, :priv, 1)
class A; private :m; end
begin
  via_public_send(A.new, :m, 1)
rescue NoMethodError => e
  p e.message
end
p via_send(A.new, :m, 1)   # send ignores visibility
# redefinition and undef seen by the cached site
class A; def n(x) = [:a_n2, x]; end
p via_send(A.new, :n, 5)
class B; undef_method :m; end
begin
  via_send(B.new, :m, 1)
rescue NoMethodError => e
  p e.message
end
# a missing name goes to method_missing
class MM; def method_missing(name, *a) = [:mm, name, a]; def respond_to_missing?(*) = true; end
p via_send(MM.new, :zzz, 1), via_send(MM.new, :zzz, 2)
# arity error through send
begin
  A.new.send(:opt)
rescue ArgumentError => e
  p e.message
end
# a raise inside the sent method keeps it in the backtrace
class R; def boom(x) = raise("boom #{x}"); end
begin
  via_send(R.new, :boom, 3)
rescue => e
  p e.message, e.backtrace_locations.map(&:label).first(2)
end
