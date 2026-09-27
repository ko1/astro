# Time.at with a negative sub-second borrows a second (ns stays in [0, 1e9)), and #to_r /
# Time.at(time) floor the epoch: -1.5 is -2 + 1/2 (was 23:59:59.-5).
[[-1, -500000], [1, -500000], [0, -1], [-1, 500000], [2, 1_500_000], [0, -1_500_000]].each do |s, us|
  t = Time.at(s, us).utc
  p [s, us, t.to_i, t.usec, t.nsec, t.to_r, t.subsec, t.inspect]
end
t = Time.at(-1, -500, :millisecond).utc; p [t.to_i, t.nsec, t.inspect]
t = Time.at(-1, -5, :nsec).utc; p [t.to_i, t.nsec, t.inspect]
t = Time.at(-1.5).utc; p [t.to_i, t.nsec, t.inspect]
t = Time.at(Rational(-3, 2)).utc; p [t.to_i, t.nsec, t.inspect]
p Time.at(-1, -500000) == Time.at(-2, 500000), (Time.at(0, -1) <=> Time.at(-1, 999999))
t = Time.at(-1, -500000).utc
p t.to_f, t.round.inspect, (t + 0.5).inspect, (t - 1).to_r, (Time.at(0) - t), t.strftime("%s %N %L"), Time.at(t).utc.inspect, Marshal.load(Marshal.dump(t)).inspect
p Time.at(-0.25).utc.inspect, Time.at(-0.25).to_r, Time.at(-1, -1, :nsec).to_r, Time.at(-100.75).utc.usec
