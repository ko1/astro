# Time keeps the part below one nanosecond exactly (CRuby's rational Time): Time.at /
# Time.utc / #+ / #- with a Rational or a Float, and #subsec / #to_r / <=> / == /
# %10N / inspect (" 2/3") / round / floor / ceil / Marshal carry it; floor / ceil /
# round refuse negative digits.
def t = (yield rescue [$!.class, $!.message])
a = Time.at(Rational(2, 3)).utc
p a.nsec, a.subsec, a.to_r, a.iso8601(10), a.strftime("%N %10N %12N %3N"), a.usec
b = Time.at(0) - Rational(1_000_000_000_000_001, 1_000_000_000_000_000)
p b.subsec, b.to_r, (Time.at(0) + Rational(999_999_999_999_999, 1_000_000_000_000_000)).subsec
p Time.at(0.7).nsec, Time.at(0.7).subsec, Time.at(1.1).subsec, Time.at(1.1).to_r, Time.at(-0.1).to_r
p Time.at(0, Rational(1, 3), :nsec).subsec, Time.at(0, 1.5, :nsec).nsec, Time.at(0, Rational(-1, 3), :usec).to_r
x = Time.at(Rational(1, 3)); y = Time.at(Rational(1, 3)); z = Time.at(0, 333333333, :nsec)
p x == y, x == z, x <=> z, z <=> x, x.eql?(y), [x, z].min.equal?(z), x.hash == y.hash
p x.getlocal("+09:00").subsec, x.utc.subsec, x.dup.subsec, (x + 1).subsec, (x - x), (x + Rational(2, 3)).subsec, (x + Rational(2,3)).to_r
p x.round(9).subsec, Time.at(Rational(2, 3)).round(9).nsec, Time.at(Rational(2, 3)).round(10).subsec, Time.at(Rational(2, 3)).floor(10).subsec, Time.at(Rational(2, 3)).ceil(10).subsec, Time.at(Rational(2,3)).floor(3).subsec, Time.at(Rational(2,3)).ceil(9).subsec
m = Marshal.load(Marshal.dump(Time.at(Rational(2, 3)).utc)); p m.subsec, m.utc?
m = Marshal.load(Marshal.dump(Time.at(Rational(2, 3)).getlocal("+09:00"))); p m.subsec, m.utc_offset
p Time.utc(2000, 1, 1, 12, 30, 0, 9999r/10000).nsec, Time.utc(2000, 1, 1, 12, 30, 0, 9999r/10000).subsec, Time.utc(2000,1,1,0,0,Rational(1,3)).subsec, Time.utc(2000,1,1,0,0,0.7).nsec
p Time.at(Rational(2, 3)).instance_variables, Time.at(Rational(2, 3)).inspect
ts = [Time.at(1234567, 987654321, :nsec).utc, Time.at(1234567, 987654321, :nsec).getlocal("+09:30"), Time.at(-3.25).utc, Time.at(Rational(10, 3)).utc, Time.utc(2020, 1, 1, 0, 0, 59.999)]
ts.each do |t|
  p [t.floor(-1), t.ceil(-1), t.round(-1)].map { |x| x } rescue p $!.message
  p [0, 1, 3, 8, 9, 10, 12].map { |n| [t.floor(n).inspect, t.ceil(n).inspect, t.round(n).inspect] }
  p t.floor.utc_offset, t.ceil(2).class, t.floor(9).equal?(t)
end
class MyT < Time; end
p MyT.at(1.5).floor.class, MyT.at(1.5).round.class
