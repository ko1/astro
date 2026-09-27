# Class#allocate builds the builtin's real payload (Set, Regexp, String), refuses
# classes without an allocator (Struct / Data themselves, Rational / Complex), and an
# allocated Regexp refuses every method until #initialize (was a SEGV in #hash).
s = Set.allocate; p s, s.size; s << 1; p s
c = Class.new(Set); t = c.allocate; t << 2; p t.size, t.class == c, t.to_a
st = String.allocate; p st.encoding, st.frozen?; st << "é"; p st, st.encoding
p((Rational.allocate rescue [$!.class, $!.message]), (Complex.allocate rescue [$!.class, $!.message]))
p Rational.respond_to?(:allocate), Complex.respond_to?(:allocate)
p((Struct.allocate rescue [$!.class, $!.message]), (Data.allocate rescue [$!.class, $!.message]))
p((Class.new(Struct).allocate rescue $!.message.sub(/\h{8,}/, "X")), Struct.new(:a).allocate)
class P < Struct.new(:x); end; p P.allocate
D2 = Data.define(:q); class D3 < D2; end; p D3.allocate
r = Regexp.allocate
p r.inspect.sub(/\h{8,}/, "X"), r.frozen?, r.encoding, r.fixed_encoding?, r == r, r.eql?(r), r == 1
%i[source options to_s hash casefold? names named_captures dup clone].each do |m|
  p [m, (r.__send__(m) rescue [$!.class, $!.message])]
end
p [(r == /a/ rescue $!.message), (/a/ == r rescue $!.message), (r === "a" rescue $!.message), (r =~ "a" rescue $!.message)]
$_ = nil; p(~r); $_ = "x"; p((~r rescue $!.message))
p((Regexp.new(r) rescue $!.message), Regexp.union(r).equal?(r))   # { r => 1 } is not refused yet (docs/todo.md)
r.__send__(:initialize, "b"); p r, r.frozen?, (r.__send__(:initialize, "c") rescue $!.class), r.class
Sub = Class.new(Regexp); q = Sub.allocate; p q.inspect.sub(/\h{8,}/, "X")
q.__send__(:initialize, "a+", "i"); p q, q.frozen?, q =~ "xA", q.class
