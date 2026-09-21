# Range#to_s / #inspect render their bounds with the bounds' OWN #to_s /
# #inspect.  koruby used the C printers without a slots region, so a user
# object (or a Date) fell back to "#<Date:0x...>" — which is what
# `rand(Date..Date)`'s ArgumentError message showed.
require 'date'
class P
  def initialize(v) = @v = v
  def to_s = "TS(#{@v})"
  def inspect = "IN(#{@v})"
  def <=>(o) = 0
end
r = P.new(1)..P.new(2)
p [r.to_s, r.inspect, "#{r}"]
p [(1..5).to_s, (1..5).inspect, (1...5).to_s]
p [(1..).to_s, (1..).inspect, (..5).to_s, (..5).inspect]
p [("a".."c").to_s, ("a".."c").inspect]
p [(nil..nil).to_s, (nil..nil).inspect]
p [[1..2].to_s, {a: 1..2}.to_s, (1..2).to_s]
p (Date.civil(2000)..Date.civil(2001)).to_s
