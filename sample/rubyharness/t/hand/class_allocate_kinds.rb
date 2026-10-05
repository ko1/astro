# Class#allocate for every kind of class: plain Object-based ones take the cached generic path, the rest keep their payloads / errors
module M1; end; module M2; end
class Plain; end
class Deep < Plain; include M1; include M2; end
St = Struct.new(:a, :b)
class StSub < St; end
Dt = Data.define(:x)
class HSub < Hash; end
class SSub < String; end
class ESub < StandardError; end
class ModSub < Module; end
class EnSub < Enumerator; end
class BO < BasicObject; end
2.times do
  p Plain.allocate.class, Deep.allocate.class, Deep.allocate.is_a?(M2), St.allocate.to_a, StSub.allocate.class
  p HSub.allocate.class, HSub.allocate.size, SSub.allocate.class, SSub.allocate.encoding, ESub.allocate.class
  p EnSub.allocate.class, BO.allocate.__id__.is_a?(Integer)
  o = Deep.allocate; o.instance_variable_set(:@x, 1); p o.instance_variable_get(:@x)
end
[-> { Integer.allocate }, -> { MatchData.allocate }, -> { Plain.singleton_class.allocate }, -> { Struct.allocate },
 -> { Data.allocate }, -> { Symbol.allocate }].each do |f|
  f.call
rescue TypeError, NoMethodError => e
  p e.class, e.message[0, 50]
end
begin
  p Dt.allocate.class
rescue => e
  p e.class
end
3.times { |i| GC.start if i == 1; a = Array.new(50) { Deep.allocate }; p a.size, a.last.class }
