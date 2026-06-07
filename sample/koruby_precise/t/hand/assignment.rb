# L0: assignment forms
a = 1
p a
a = b = 2
p [a, b]
x, y = 1, 2
p [x, y]
x, y = y, x
p [x, y]
a, b, c = [10, 20, 30]
p [a, b, c]
first, *rest = [1, 2, 3, 4]
p first
p rest
*init, last = [1, 2, 3, 4]
p init
p last
a, (b, c), d = 1, [2, 3], 4
p [a, b, c, d]
n = 5
n += 3
p n
n -= 1
p n
n *= 2
p n
n /= 3
p n
n **= 2
p n
s = "a"
s += "b"
p s
s *= 3
p s
arr = [1, 2, 3]
arr[0] += 10
p arr
h = { x: 1 }
h[:x] += 5
p h
flag = nil
flag ||= "default"
p flag
flag ||= "other"
p flag
val = 5
val &&= val + 1
p val
m, = [1, 2]
p m
