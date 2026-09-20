require "date"
# DateTime の offset は「日の端数」。Date._parse は秒で返すので変換が要る。
# 1 桁の時 ("+9")、秒付き ("+09:00:30")、ゾーン名 ("JST") も受ける。
offs = ["+9", "+09", "+09:00", "+0900", "-5", "-05:30", "Z", "UTC", "GMT",
        "+09:00:30", "+090030", "JST", "EST", "z", "+00:00", "-0",
        9, 0, Rational(3, 8), 0.375]
offs.each do |o|
  begin
    d = DateTime.new(2018, 1, 14, 22, 25, 19, o)
    puts "#{o.inspect} -> #{d.offset.inspect} #{d.strftime('%z')}"
  rescue => e
    puts "#{o.inspect} -> #{e.class}"
  end
end
["2018-01-14T22:25:19+09:00", "2018-01-14T22:25:19Z", "2018-01-14T22:25:19-05:00",
 "2018-01-14T22:25:19", "2018-01-14T22:25:19.1+09:00"].each do |s|
  d = DateTime.parse(s)
  puts "#{s} -> #{d.strftime('%FT%T%:z')} off=#{d.offset} frac=#{d.sec_fraction}"
end
p DateTime.strptime("2018-01-14T22:25:19+09:00", "%FT%T%z").offset
p DateTime.iso8601("2018-01-14T22:25:19.25+09:00").sec_fraction
