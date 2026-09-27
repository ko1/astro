# Date._parse / strptime / strftime and time.rb are ports of ruby/date's C
# parser and ruby/time: free-form strings, zones, week dates, the RFC/ISO
# families and their formatting agree with CRuby.
require "date"
require "time"
ENV["TZ"] = "UTC"
%w[2026-09-27 27/9/2026 Sep\ 27\ 2026 Sun,\ 27\ Sep\ 2026\ 10:11:12\ +0900 H31.04.30 R01.05.01
   2026-W39-7 2026-270 20260927T101112Z 10:11pm\ EST 3rd\ Feb\ 2001 -0001-01-01 12/31/99
   Tuesday 2026-09-27T10:11:12.345+05:30 Jan\ 2\ 3:04:05\ PM\ JST\ 2006].each do |s|
  p [s, Date._parse(s)]
end
p Date.parse("2026-09-27 +09:00"), DateTime.parse("2001-02-03T04:05:06.123+07:00").sec_fraction
p Date._strptime("2001-W05-6", "%G-W%V-%u"), Date._strptime("x 12", "%y"), Date._strptime("2001 33", "%Y %U")
p Date.strptime("2001 5 3", "%Y %U %w"), DateTime.strptime("1000000000 +0900", "%s %z"), Date._strptime("12:34 pm", "%I:%M %p")
d = DateTime.new(2001, 2, 3, 4, 5, 6.5r, "+09:00")
p d.strftime("%-m/%e %^a %10A %:z %::z %s %N %3N %Q %%"), d.iso8601(3), d.rfc3339, d.jisx0301(2), d.httpdate, d.rfc2822
p Date.new(1912, 7, 30).jisx0301, Date.new(1926, 12, 25).jisx0301, Date.new(2019, 5, 1).jisx0301
p Time.parse("2026-09-27 10:11:12 +09:00"), Time.iso8601("2026-09-27T10:11:12.5Z").subsec, Time.rfc2822("Sun, 27 Sep 2026 10:11:12 -0000")
p Time.httpdate("Sun, 27 Sep 2026 10:11:12 GMT"), Time.strptime("27/09/2026 10", "%d/%m/%Y %H"), Time.xmlschema("2026-09-27T10:11:12+01:00").utc_offset
p Time.utc(2001, 4, 17, 19, 23, 17, 300000).xmlschema(1.9), Time.utc(2001, 4, 17).httpdate, Time.at(0).utc.rfc2822
p (Date.parse("garbage") rescue $!.class), (Date.strptime("x", "%Y") rescue $!.class), (Time.parse("") rescue $!.class)
p Date._parse("x" * 200, limit: 100) rescue p $!.class
p Date._iso8601("2026-09-27T10:11:12Z"), Date._rfc2822("Sun, 27 Sep 2026 10:11:12 -0000"), Date._httpdate("Sunday, 27-Sep-26 10:11:12 GMT")
