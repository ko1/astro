# the C dumper writes instances of classes the Ruby dumper already wrote as plain objects, byte for byte
# (links, cycles, nesting); extended / singleton objects and a later marshal_dump / _dump go back to Ruby
module NS; class Inner; def initialize(v) = (@v = v); end; end
class Pt; def initialize(x, y) = (@x = x; @y = y); end
class Late; def initialize = (@a = 1); end
class Sub < Pt; end
S = Struct.new(:m)
def show(label, obj)
  2.times do |i|
    b = begin; Marshal.dump(obj); rescue => e; "#{e.class}: #{e.message.sub(/0x\h+/, 'X')}"; end
    puts "#{label}/#{i}: #{b.is_a?(String) && b.encoding == Encoding::BINARY ? b.unpack1('H*') : b}"
  end
end
show :plain, Pt.new(1, "s")
show :nested, Pt.new(NS::Inner.new([1, :sym]), {k: NS::Inner.new(nil)})
shared = NS::Inner.new(2); show :shared, [shared, Pt.new(shared, shared)]
cyc = Pt.new(nil, nil); cyc.instance_variable_set(:@x, cyc); show :cycle, cyc
show :noivars, Late.allocate
show :sub, Sub.new(1.5, 2**70)
e = Pt.new(1, 2); e.extend(Comparable); show :extended, e
sg = Pt.new(1, 2); def sg.hi = 1; show :singleton, sg
show :anon, Class.new.new
show :struct_in, Pt.new(S.new(1), nil)
show :late_before, Late.new
class Late; def marshal_dump = [9]; def marshal_load(a) = (@a = a); end
show :late_mdump, Late.new
class NS::Inner; def _dump(_l) = "raw"; def self._load(s) = new(s); end
show :late_udump, NS::Inner.new(3)
x = Marshal.load(Marshal.dump(Pt.new(Pt.new(1, 2), [3]))); p x.instance_variable_get(:@x).instance_variable_get(:@y)
o = Object.new; o.instance_variable_set(:@z, "é"); show :object, o
D = Pt.dup
show :dup_named, D.new(1, 2)
anon = Pt.dup
show :dup_anon, anon.new(1, 2)
