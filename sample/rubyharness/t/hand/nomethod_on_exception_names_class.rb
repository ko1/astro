# NoMethodError on an exception names its class.
class Foo < StandardError; end
module M; class E2 < RuntimeError; end; end
[Foo.new, M::E2.new, RuntimeError.new, (raise ArgumentError rescue $!), Class.new(StandardError).new].each { |e| begin; e.zzz; rescue NoMethodError => x; p x.message.sub(/0x\h+/, "0x") end }
