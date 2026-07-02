# Time day-of-week predicates + Time#to_a. vs ruby.
t = Time.new(2020, 6, 15, 10, 30, 45)  # a Monday
p [t.sunday?, t.monday?, t.tuesday?, t.wednesday?, t.thursday?, t.friday?, t.saturday?]
p Time.new(2020, 6, 14).sunday?
p Time.new(2020, 6, 20).saturday?
a = t.to_a
p a[0, 8]
p a.length
p a[6]
p a[7]
