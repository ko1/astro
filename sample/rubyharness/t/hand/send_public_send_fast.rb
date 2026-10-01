# obj.send(:m, ...) / obj.public_send(:m, ...) / __send__ resolved and dispatched in place:
# private and protected methods, no args, keywords, arity errors, method_missing, a class
# with its own #send, string names, nested send, builtin receivers, backtraces.
# Run under both and diff.
class V
  def pub(a, b = 2, k: 3) = [a, b, k]
  def none = :none
  private def priv = :priv
  protected def prot = :prot
  def method_missing(n, *a) = n == :ghost ? [:mm, a] : super
  def respond_to_missing?(n, p = false) = n == :ghost || super
  def boom = raise("boom")
end
v = V.new
p v.send(:pub, 1), v.public_send(:pub, 1, 5, k: 9), v.__send__(:none), v.send(:priv)
begin; v.public_send(:priv); rescue NoMethodError => e; p e.message[/private method 'priv'/]; end
begin; v.public_send(:prot); rescue NoMethodError => e; p e.message[/protected method 'prot'/]; end
p v.send(:prot)
begin; v.send(:pub); rescue ArgumentError => e; p e.message; end
p v.send(:ghost, 1), v.public_send(:ghost)
p v.send("pub", 7), v.send(:send, :none), v.public_send(:public_send, :none)
p [3, 1, 2].send(:sort), "ab".public_send(:upcase), 5.send(:+, 1), { a: 1 }.send(:fetch, :a)
class Sock
  def send(*a) = [:own_send, a]
end
p Sock.new.send(:x, 1)
begin; v.send(:boom); rescue => e; p e.message, e.backtrace.first[/in '.*'/]; end
begin; v.public_send(:nope); rescue NoMethodError => e; p e.name; end
3.times { |i| p v.public_send(:pub, i) }
