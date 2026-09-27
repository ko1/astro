# #new stays undefined in subclasses of Integer / Rational (lookup up the chain), and a
# singleton `def Integer.new` answers it again.
c = Class.new(Integer); p c.respond_to?(:new), (c.new rescue [$!.class, $!.message.sub(/#<Class:0x\h+>/, "C")])
class MyRat < Rational; end rescue p $!
p Integer.respond_to?(:new), (Integer.new rescue $!.message), Rational.respond_to?(:new), (Rational.new(1) rescue $!.message)
def Integer.new(*) = :ok
p Integer.respond_to?(:new), Integer.new, Integer.send(:new), Integer.public_send(:new), (Integer.new { } rescue $!.class), c.new, c.respond_to?(:new)
p Float.respond_to?(:new), Class.new(Float).respond_to?(:new), Object.respond_to?(:new), Class.new.new.class.class
