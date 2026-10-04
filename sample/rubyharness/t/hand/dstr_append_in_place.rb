# string interpolation appends in place after its first step: results, encodings and object identity stay as before
# encoding: utf-8
a = "x"; b = 12; f = 1.5; s = :sym; u = "é"
r = "#{a}-#{b}-#{f}-#{s}-#{nil}-#{true}-#{false}-#{u}"
p r, r.encoding, r.frozen?
p "#{b}".encoding, "#{a}#{b}".encoding, "pre#{b}post".encoding
lit = "lit"
x = "#{lit}"
p x.equal?(lit), x, lit
x << "!"
p lit                                   # the source part is never the accumulator
parts = 3.times.map { |i| "<#{i}:#{a * i}>" }
p parts, parts.map(&:object_id).uniq.size
class Obj; def to_s = "OBJ"; end
p "#{Obj.new}|#{[1, "two", :three]}|#{{k: 1}}|#{1..3}"
p "#{/re/}", "#{Object}", "#{Comparable}"
ascii = "#{b}".force_encoding("US-ASCII")
p "#{ascii}#{u}".encoding, "x#{ascii}#{b}".encoding
bin = "\xff".b
p "#{bin}#{b}".encoding, "#{bin}#{b}".bytes
begin
  "#{u}#{"\xff".force_encoding('Shift_JIS')}"
rescue => e
  p e.class
end
p :"sym#{b}", /a#{b}c/.source
h = <<~EOS
  line #{a}
  next #{b}
EOS
p h
acc = 0
1000.times { |i| GC.start if i % 250 == 0; acc += "#{i}-#{a}-#{i * 2}".size }
p acc
