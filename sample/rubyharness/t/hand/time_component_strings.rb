# Time component Strings: a month name must be exactly 3 letters, anything else is
# Integer(str) (invalid → ArgumentError naming the String inspected, huge → RangeError);
# Integer()'s own message inspects the String too ("1\\u0000").
def t = (yield rescue [$!.class, $!.message])
p t { Time.new(2020, "mayhem", 1, 0, 0, 0, "+00:00") }
p t { Time.new("1979\0junk", 1, 1, 0, 0, 0, "+00:00") }
p t { Time.new(2020, "may", 1, 0, 0, 0, "+00:00") }, t { Time.new(2020, "MAY", 1) }, t { Time.new(2020, "05", 1, 0, 0, 0, "+00:00") }, t { Time.new("1979", 1, 1, 0, 0, 0, "+00:00") }
p t { Time.new(2020, "5x", 1) }, t { Time.new(2020, 5, "1x") }, t { Time.new(2020, 5, 1, "2\0") }, t { Time.new(2020, " 5", 1) }, t { Time.new(2020, "+5", 1) }, t { Time.new(2020, "", 1) }
p t { Time.new(2020, 5, 1, 0, 0, 0) }, t { Time.new(2020, "may", 1).month }, t { Time.utc(2020, "mayhem", 1) }, t { Time.local(2020, "jun", 1).month }, t { Time.gm(2000, "Jan", 1).month }
p t { Integer("1\0") }, t { Integer("1\0", 10) }, Integer("x", exception: false), t { Time.new(2020, "1" * 30, 1) }, t { Time.new(2020, "1_2", 1).month }
