# control flow: if / while / return

# basic if
if true
  p "t"
end
if false
  p "F"
else
  p "f"
end

# elsif (= nested else if)
x = 2
if x == 1
  p "one"
else
  if x == 2
    p "two"
  else
    p "other"
  end
end

# truthiness: 0 / "" / [] are truthy, nil / false are falsy
if 0
  p "0 truthy"
end
if ""
  p "'' truthy"
end
if []
  p "[] truthy"
end
if nil
  p "nil F"
else
  p "nil f"
end
if false
  p "false F"
else
  p "false f"
end

# while
i = 0
sum = 0
while i < 10
  sum = sum + i
  i = i + 1
end
p sum                  # 45
p i                    # 10

# nested while
m = []
i = 0
while i < 3
  row = []
  j = 0
  while j < 3
    row.push(i * 10 + j)
    j = j + 1
  end
  m.push(row)
  i = i + 1
end
p m

# return in method
def early(n)
  if n < 0
    return -1
  end
  if n == 0
    return 0
  end
  1
end
p early(-5)
p early(0)
p early(7)

# return from while
def find_first_neg(a)
  i = 0
  while i < a.size
    if a[i] < 0
      return a[i]
    end
    i = i + 1
  end
  nil
end
p find_first_neg([1, 2, -3, 4])
p find_first_neg([1, 2, 3])
