# strftime %Y / %F / %c / %v, inspect, to_s, ctime and xmlschema write years below 1000
# and negative years as CRuby (0005, -0001); %s is the epoch whatever the TZ.
[-1, 5, 999, 2020, 12345, -12345].each { |y| t = Time.utc(y); p [t.strftime("%Y|%6Y|%-Y|%_6Y|%06Y|%2Y|%-6Y|%v|%F|%x|%c"), t.inspect, t.to_s, t.ctime, t.xmlschema, t.iso8601(3)] }
t = Time.new(5, 1, 1, 0, 0, 0, "+09:00"); p t.inspect, t.to_s, t.ctime
e = Time.at(100).utc; p e.strftime("%s|%10s|%-10s|%_10s|%010s"), Time.at(-5).utc.strftime("%s|%6s|%_6s"), Time.at(100).getlocal("+05:00").strftime("%s")
