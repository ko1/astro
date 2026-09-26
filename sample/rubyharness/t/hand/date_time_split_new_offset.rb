# Date answers no time-of-day / zone (DateTime does), a frozen Date still works,
# DateTime#new_offset keeps the instant, DateTime.jd keeps a day fraction.
require 'date'
d = Date.new(2008, 3, 1)
p [:hour, :min, :sec, :offset, :zone, :new_offset, :day_fraction].map { d.respond_to?(_1) }, d.send(:hour)
p [:hour, :min, :sec, :offset, :zone, :new_offset].map { DateTime.now.respond_to?(_1) }
p Date.new(2012, 1, 1).freeze.to_s, Date.new(2012, 1, 1).freeze.year, (Date.new(2012, 1, 1).freeze + 1).to_s
dt = DateTime.new(2017, 1, 1, 12, 0, 0, "+09:00")
p dt.new_offset(0).to_s, dt.new_offset("-05:00").to_s, dt.new_offset.to_s, dt.new_offset(Rational(3, 24)).to_s, dt.offset, dt.zone
p dt.new_offset(0) == dt, DateTime.jd(Rational(3, 2)).day_fraction, DateTime.jd(Rational(3, 2)).to_s, DateTime.new(2017, 1, 1, 23, 0, 0, "-02:00").new_offset("+03:00").to_s
