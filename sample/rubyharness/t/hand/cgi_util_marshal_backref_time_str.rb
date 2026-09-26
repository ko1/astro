# cgi/util loads (a shim over cgi/escape); Marshal finds an object from inside its
# own ivars; Time.new with a String component and an offset parses it.
require "cgi/util"
p CGI.escapeHTML("<a>"), CGI.escape("a b")
o = Object.new; o.instance_variable_set(:@a, [o]); o.instance_variable_set(:@b, {k: o})
l = Marshal.load(Marshal.dump(o))
p l.instance_variable_get(:@a)[0].equal?(l), l.instance_variable_get(:@b)[:k].equal?(l)
S = Struct.new(:x); s = S.new; s.x = [s]; ls = Marshal.load(Marshal.dump(s)); p ls.x[0].equal?(ls)
e = RuntimeError.new("m"); e.instance_variable_set(:@z, 1); p Marshal.load(Marshal.dump(e)).instance_variable_get(:@z)
p Time.new("1979", 5, 27, 0, 0, 0, "+00:00").year, Time.new("1979", "may", "27", "1", "2", "3", "+09:00").to_a.first(6)
p((Time.new("x", 1, 1, 0, 0, 0, "+00:00") rescue $!.class))
