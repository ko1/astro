# Time.new with a utc_offset (Integer seconds or "±HH[:MM[:SS]]"/"UTC"). vs ruby.
t = Time.new(2000, 1, 1, 12, 0, 0, 3600)
p t.utc_offset
p t.hour
p t.min
p t.to_i
p Time.new(2000, 1, 1, 0, 0, 0, "+05:30").utc_offset
p Time.new(2000, 1, 1, 0, 0, 0, "-05:00").utc_offset
p Time.new(2000, 1, 1, 0, 0, 0, "+09").utc_offset
p Time.new(2000, 1, 1, 0, 0, 0, "+05:30:15").utc_offset
p Time.new(2000, 1, 1, 0, 0, 0, "UTC").utc_offset
class OffToI; def to_int; -7200; end; end
p Time.new(2000, 1, 1, 0, 0, 0, OffToI.new).utc_offset
begin; Time.new(2000,1,1,0,0,0, 90000); rescue ArgumentError; p :out_of_range; end
