# Fixes for lines the rubyharness corpus reported against CRuby master.

# a BasicObject subclass does not see Object's constants; modules do
class BOsub < BasicObject; end
p BasicObject.const_defined?(:Kernel), BOsub.const_defined?(:String), Module.new.const_defined?(:String)
p((BOsub.const_get(:String) rescue $!.class), Comparable.const_get(:String))

# a capture inside a lookahead (and the absent operator) is not linear time; lookbehind is
%w[x+(?=(a)) x+(?=a) (?=(?<n>a)) .(?<=(a)) (?~ab) (?>a+)b (?=[(]) (a)\\1].each { |s| print Regexp.linear_time?(Regexp.new(s)), " " }
puts

# IO#wait / time interval messages name nil / true literally
r, w = IO.pipe
begin; r.wait(nil, 0); rescue TypeError => e; p e.message; end
begin; r.wait(:r, nil, nil); rescue => e; p e.class; end
begin; r.timeout = true; rescue TypeError => e; p e.message; end
r.close; w.close

# the built-in globals, and no koruby-internal ones
g = global_variables
p g.size >= 42, g.include?(:$-i), g.include?(:$FILENAME), g.grep(/\A\$__/)

# CESU-8: an astral character is a surrogate pair of 3-byte units, one character
s = 0x10400.chr(Encoding::CESU_8)
p s.bytes, s.length, s.ord

# String#succ leaves invalid bytes alone and steps the rightmost valid character
p "(\xFF".succ, "\xFF".succ, "\xFF\xFF".succ, "<\xFF\xFF".succ, "(\xFF)".succ, "\xFF".b.succ

# Time.new(string): precision nil / negative keeps every digit, the default keeps nine
t = "2021-12-25 00:00:00.123456789876 +09:00"
p Time.new(t).subsec, Time.new(t, precision: nil).subsec, Time.new(t, precision: -1).subsec, Time.new(t, precision: 3).subsec
p Time.new("2020-12-24T15:56:17Z").utc?
