# "#{scalar}" formats Integer/Float/Symbol/nil/true/false in C only while their #to_s is the builtin one
x = 42; f = 1.5; s = :sym; n = nil
p "a#{x}b", "#{f}", "#{s}!", "[#{n}]", "#{true}/#{false}", "#{-7}#{x}", "#{10**20}", "#{2**62}"
p "#{x}".encoding, "é#{x}".encoding, "#{:é}".encoding, "#{x}é".encoding, "#{s}".encoding, "#{n}".encoding
p "#{x}".frozen?, "#{x}#{x}".size
class Foo; def to_s = "foo!"; end
p "#{Foo.new}", "#{x}"                     # a user class's to_s does not change Integer's
module Loud; def to_s = "LOUD#{super}"; end
class Symbol; prepend Loud; end
p "#{:abc}", "#{x}"
class Integer
  alias __orig_to_s to_s
  def to_s(*a) = "int(#{__orig_to_s(*a)})"
end
p "#{x}", "#{f}", 5.to_s
class Integer
  alias to_s __orig_to_s
end
p "#{x}"
module Ref
  refine NilClass do
    def to_s = "NIL"
  end
end
p "#{nil}"
using Ref
p "#{nil}", "#{x}"
class Float; def to_s = "F"; end
p "#{f}", "#{x}"
class TrueClass; def to_s = "yes"; end
p "#{true}", "#{false}"
3.times { |i| GC.start if i == 1; p "#{i}:#{:k}:#{nil}" }
