# Time zone/offset/getutc/round/eql? + out-of-range validation. vs ruby (UTC-based).
u = Time.utc(2020, 6, 15, 10, 30, 45)
p u.zone
p u.utc_offset
p u.gmt_offset
p u.utc?
p u.getutc.utc?
p u.round.class
p u.eql?(Time.utc(2020, 6, 15, 10, 30, 45))
p u.eql?(Time.utc(2020, 6, 15, 10, 30, 46))
p u == Time.utc(2020, 6, 15, 10, 30, 45)
begin; Time.new(2020, 13, 1); rescue ArgumentError => e; p [:month, e.class]; end
begin; Time.new(2020, 6, 32); rescue ArgumentError => e; p [:day, e.class]; end
begin; Time.new(2020, 6, 15, 25); rescue ArgumentError => e; p [:hour, e.class]; end
begin; Time.new(2020, 6, 15, 10, 61); rescue ArgumentError => e; p [:min, e.class]; end
