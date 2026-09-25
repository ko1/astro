# `Date.parse` accepts a comma right after an ordinal day ("18th, November
# 1983").  koruby's separator class for that branch was [.\s/] with no comma, so
# the day was dropped and the 1st of the month came back instead.  transproc's
# coercion spec parses exactly that string.
require "date"

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

[
  "18th, November 1983", "18th November 1983", "18 November 1983",
  "1st, Jan 2000", "2nd,Feb 2001", "3rd , Mar 2002", "November 18, 1983",
  "18th. November 1983", "18th-November-1983", "22nd, december 1999",
  "5th, Jun 2020 10:11:12", "2005-11-05", "Nov 18 1983", "18/11/1983",
  "1983-11-18T01:02:03+09:00", "31st, December 2099", "4th, may 1970"
].each { |s| t(s) { Date.parse(s).to_s } }

t(:datetime)  { DateTime.parse("5th, Jun 2020 10:11:12").to_s }
t(:time)      { require "time"; Time.parse("18th, November 1983").to_i }
# Date._parse's KEY ORDER differs (CRuby emits time fields first, then
# year/mon/mday; koruby's regex branches fill them as they match), so compare
# the contents, not the insertion order.
t(:_parse)    { Date._parse("18th, November 1983").sort_by { |k, _| k.to_s }.to_h }
t(:bad)       { Date.parse("not a date") }
t(:empty)     { Date.parse("") }
