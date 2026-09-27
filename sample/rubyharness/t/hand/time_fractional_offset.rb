t = Time.gm(2007, 1, 9, 12, 0, 0)
[Rational(-7201, 2), Rational(7201, 2), Rational(1, 3)].each do |o|
  g = t.getlocal(o); p [g, g.hour, g.min, g.sec, g.strftime("%H:%M:%S %z"), g.to_a.take(6)]
  n = Time.new(2007, 1, 9, 13, 0, 0, o); p [n, n.utc, n.to_r]
end
p Time.at(0, in: "+09:00:01"), Time.new(2000, 1, 1, in: "-00:00:30")
