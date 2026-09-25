# Time.iso8601 / xmlschema / parse / rfc2822 keep a numeric offset as a
# fixed-offset time (only Z / UTC / -00:00 give UTC); Date._parse knows zone names.
require "time"
t = Time.iso8601("2026-09-25T23:07:15.123+00:00"); p t.iso8601, t.utc?, t.zone, t.utc_offset
t2 = Time.iso8601("2026-09-25T23:07:15Z"); p t2.iso8601, t2.utc?
t3 = Time.iso8601("2026-09-25T23:07:15+09:00"); p t3.iso8601, t3.utc_offset, t3.hour
t4 = Time.xmlschema("2026-09-25T23:07:15.5-05:30"); p t4.iso8601(3), t4.utc_offset
p Time.parse("2026-09-25 10:00:00 +0000").utc?, Time.strptime("2026-09-25T10:00:00+00:00", "%Y-%m-%dT%H:%M:%S%z").utc?
["2026-09-25 10:00:00 UTC", "2026-09-25 10:00:00 GMT", "2026-09-25T10:00:00-00:00", "Thu, 25 Sep 2026 10:00:00 +0900", "2026-09-25 10:00 PST", "2026-09-25 10:00:00"].each do |s|
  t = Time.parse(s); p [t.utc?, t.utc_offset == 0 ? 0 : t.utc_offset, t.hour]
end
p Time.rfc2822("Thu, 25 Sep 2026 10:00:00 +0900").utc_offset, Time.httpdate("Thu, 25 Sep 2026 10:00:00 GMT").utc?
["2026-09-25 10:00 PST", "Sat Aug 28 02:55:50 +09:00 1999", "10:00 JST", "2026-09-25 est", "Mar 3 2026", "Wed, 01 Jan 2026 00:00:00 EDT", "2026-09-25T10:00:00 CEST"].each { |x| p Date._parse(x).slice(:zone, :offset) }
