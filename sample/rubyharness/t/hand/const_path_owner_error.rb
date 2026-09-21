# `class A::B::C` with an unresolvable namespace is a NameError naming the first
# missing segment, not "nil is not a class/module" (which `rescue NameError`
# cannot catch).  A namespace that resolves to a non-module value stays a
# TypeError.
module M1; end
X1 = 1
N1 = nil

[
  "class X1::Y; end",
  "module X1::Y; end",
  "class Zz::Y; end",
  "module Zz::Y; end",
  "class M1::A::B; end",
  "module M1::A::B; end",
  "class N1::Y; end",
  "class ::Nope::Y; end"
].each do |src|
  begin
    eval(src)
    puts "#{src} -> ok"
  rescue => e
    puts "#{src} -> #{e.class}: #{e.message}"
  end
end

module Wrap2
  module M1; end
  def self.go
    eval("class M1::A::B; end")
  rescue => e
    puts "nested -> #{e.class}: #{e.message}"
  end
end
Wrap2.go

# the resolvable shapes still work
module Z1; end
class Z1::W; end
p Z1::W
module Z1::V; end
p Z1::V
p Z1.constants.sort
