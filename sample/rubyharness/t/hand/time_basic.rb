# Time class — deterministic cases (no Time.now), UTC to avoid tz dependence
t = Time.utc(2000, 1, 2, 3, 4, 5)
p [t.year, t.month, t.day, t.hour, t.min, t.sec]
p [t.wday, t.yday]
p t.to_i
p t.utc?
t2 = Time.at(t.to_i).utc
p (t2.to_i == t.to_i)
p (t2.year)
p (t + 3600).hour
p (t - 5).sec
p ((t + 90) - t)
p (Time.utc(2020, 1, 1) < Time.utc(2020, 1, 2))
p (Time.utc(2020, 1, 1) == Time.utc(2020, 1, 1))
p t.strftime("%Y-%m-%d %H:%M:%S")
p Time.at(0).utc.year
p Time.utc(1970, 1, 1).to_i
