# Integer: sqrt #to_int, pow(e, m) messages, floor/ceil/round with a huge
# negative precision.
o = Object.new
def o.to_int = 10
p Integer.sqrt(o)
begin
  Integer.sqrt("test")
rescue TypeError
  puts "TypeError"
end

[[5, 12.0], [5, Rational(12, 1)], [5, "12"], [5, nil], [5, []]].each do |e, m|
  begin
    2.pow(e, m)
  rescue TypeError => ex
    puts ex.message
  end
end

[1, -1, 123456, -123456, 0].each do |n|
  p [n.floor(-20), n.ceil(-20), n.round(-20), n.truncate(-20), n.floor(-25), n.ceil(-30)]
end
p 18.floor(-1), 18.ceil(-1), 15.round(-1), -25.round(-1)
