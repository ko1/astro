# symbol subject
def kind(t)
  case t
  when :opts then "options"
  when :driver then "drv"
  when :int then "integer"
  else "other"
  end
end
p kind(:opts)
p kind(:int)
p kind(:zzz)

# multiple values per when + integer subject
def grade(n)
  case n
  when 0, 1, 2 then "low"
  when 3, 4 then "mid"
  else "high"
  end
end
p [0,1,2,3,4,5].map { |x| grade(x) }

# string subject (content ===)
case "hello"
when "hi" then p(:hi)
when "hello" then p(:hello)
end

# Class / Range / no-else
def describe(x)
  case x
  when Integer then "int"
  when String then "str"
  when 100..200 then "range"
  end
end
p describe(5)
p describe("a")
p describe(nil)

# subject evaluated once (side effect)
$n = 0
def bump; $n += 1; :a; end
case bump
when :a then p "matched a"
end
p $n

# subjectless case
x = 7
case
when x < 5 then p "small"
when x < 10 then p "medium"
else p "big"
end
