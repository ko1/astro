# Batch 3: recursive Hash#hash, object_id near the Fixnum limit, Time.local
# isdst, timezone objects answering with a fixed-offset Time.
h = {}; h[:x] = h
p h.hash == { x: h }.hash, h.hash == { x: { x: h } }.hash
h2 = {}; rec = [h2]; h2[:x] = rec
p h2.hash == { x: rec }.hash, h2.hash == { x: [h2] }.hash
p({ a: 1 }.hash == { a: 1 }.hash, { a: 1 }.hash == { a: 2 }.hash)

p (2**62 - 1).__id__, (-(2**62)).__id__, 1.__id__, -1.object_id
p (2**62 - 1).__id__ != (2**62).__id__

old = ENV["TZ"]
ENV["TZ"] = "America/New_York"
p Time.local(0, 30, 1, 30, 10, 2005, 0, 0, true, ENV['TZ']).utc_offset
p Time.local(0, 30, 1, 30, 10, 2005, 0, 0, false, ENV['TZ']).utc_offset
p Time.local(2005, 6, 1).utc_offset
ENV.delete("TZ")
ENV["TZ"] = old if old

zone = Object.new
def zone.utc_to_local(t)
  Struct.new(:year, :mon, :mday, :hour, :min, :sec, :isdst, :to_i, :zone, :utc_offset)
       .new(t.year, t.mon, t.mday, t.hour, t.min, t.sec, t.isdst, t.to_i, 'Asia/Tokyo', 9 * 60 * 60)
end
p Time.now(in: zone).utc_offset

o = Object.new
def o.utc_to_local(t) = Time.new(2007, 1, 9, 13, 0, 0, 3600)
t = Time.gm(2007, 1, 9, 12, 0, 0)
t.localtime(o)
p t == Time.new(2007, 1, 9, 13, 0, 0, 3600), t.utc_offset, t.hour
p Time.gm(2007, 1, 9, 12, 0, 0).getlocal(o).utc_offset

z2 = Object.new
def z2.local_to_utc(t) = Time.utc(t.year, t.mon, t.mday, t.hour - 2, t.min, t.sec)
def z2.utc_to_local(t) = Time.utc(t.year, t.mon, t.mday, t.hour + 2, t.min, t.sec)
tt = Time.new(2020, 5, 5, 10, 0, 0, z2)
p tt.utc_offset, tt.hour, tt.utc.hour
