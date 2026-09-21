# The broken-down time cast the epoch double to time_t, which truncates TOWARD
# ZERO: a pre-1970 stamp with a sub-second part came out one second late
# (Time.utc(1969,12,31,19,1,40,101) rendered 19:01:41).  #to_i already floored,
# so the accessors disagreed with #inspect.  rspec-support's object_formatter
# spec pins exactly this Time.
def show(t) = [t.inspect, t.to_s, t.to_i, t.sec, t.min, t.hour, t.usec, t.nsec, t.subsec]
[
  Time.utc(1969, 12, 31, 19, 1, 40, 101),
  Time.utc(1969, 12, 31, 19, 1, 40),
  Time.utc(1969, 12, 31, 19, 1, 40, 1),
  Time.utc(1969, 12, 31, 19, 1, 40, 999_999),
  Time.utc(1970, 1, 1, 0, 0, 0, 1),
  Time.utc(1900, 1, 1, 0, 0, 0, 500_000),
  Time.at(0), Time.at(-1), Time.at(1),
  Time.at(-1, 500_000), Time.at(-2, 500_000), Time.at(-1.5), Time.at(-2.5),
  Time.at(-17_900), Time.at(-17_900, 101),
  Time.at(-86_400 * 365 * 5, 1),
].each { |t| p show(t) }

# the same through a fixed offset and through #getlocal / #utc
t = Time.utc(1969, 12, 31, 19, 1, 40, 101)
p [t.getutc.inspect, t.utc?, t.gmt_offset]
p [Time.at(-1, 500_000).utc.inspect, Time.at(-1, 500_000).getutc.sec]
p Time.new(1969, 12, 31, 19, 1, 40, "+00:00").inspect
p Time.new(1969, 12, 31, 19, 1, 40, "-05:00").inspect
p Time.at(-17_900, in: "+09:00").inspect

# arithmetic across the epoch
p [(Time.at(0) - 1.5).inspect, (Time.at(0) + -1.5).inspect, (Time.at(-1) - 1).inspect]
p [(Time.at(-1, 500_000) <=> Time.at(-1)), Time.at(-1, 500_000) - Time.at(-2)]
p [Time.at(-1.5).strftime("%Y-%m-%d %H:%M:%S.%6N"), Time.at(1.5).strftime("%H:%M:%S.%6N")]
p [Time.at(-1.5).yday, Time.at(-1.5).wday, Time.at(-1.5).year, Time.at(-1.5).month, Time.at(-1.5).day]
