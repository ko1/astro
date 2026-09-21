# `undef :send` (and :__send__ / :public_send / :new) raised NameError because
# those names are special-dispatched and live in no method table.  rspec-core's
# OutputWrapper spec does `class << output; undef :send; end` to check that the
# wrapper does not rely on #send.
require 'stringio'
$VERBOSE = nil   # koruby's warnings carry no "file:line:" prefix yet
def clean(x) = x.is_a?(Array) ? [x[0], x[1].sub(/0x\h+/, "0xA")] : x

o = StringIO.new
class << o
  undef :send
end
p o.respond_to?(:send)
p clean((o.send(:puts, "x") rescue [$!.class, $!.message]))
o.puts "ok"
p o.string
p o.__send__(:string)
p o.public_send(:string)

a = Object.new
class << a
  undef :__send__
end
p [a.respond_to?(:__send__), a.respond_to?(:send), a.send(:to_s).class]
p clean((a.__send__(:to_s) rescue [$!.class, $!.message]))

class C
  undef :public_send
end
c = C.new
p [c.respond_to?(:public_send), c.respond_to?(:send)]
p clean((c.public_send(:to_s) rescue [$!.class, $!.message]))
p c.send(:to_s).class

# (`undef :new` is accepted but not honoured: its dispatch is the hot path.)

# an untouched object keeps all four
e = Object.new
p [e.respond_to?(:send), e.respond_to?(:__send__), e.respond_to?(:public_send), Object.respond_to?(:new)]
p [e.send(:class), e.__send__(:class), e.public_send(:class)]

# a class that defines its OWN #send still wins
class F
  def send(*args) = [:own, args]
end
p F.new.send(1, 2)
