# GC ストレス: 大量 alloc + push/realloc + 長 chain

# 配列を grow させまくる (= 多数の realloc を発火)
def grow(n)
  a = []
  i = 0
  while i < n
    a.push(i)
    i = i + 1
  end
  a
end

a = grow(1000)
p a.size
p a[0]
p a[500]
p a[999]

# nested alloc (= alloc 中に alloc)
def nested(n)
  outer = []
  i = 0
  while i < n
    inner = [i, i + 1, i + 2]
    outer.push(inner)
    i = i + 1
  end
  outer
end
b = nested(100)
p b.size
p b[50]
p b[99]

# string concat chain (= 短命 string 多数生成)
def chain(n)
  s = ""
  i = 0
  while i < n
    s = s + "x"
    i = i + 1
  end
  s
end
c = chain(100)
p c.size

# mixed alloc + free
def mixed(n)
  acc = 0
  i = 0
  while i < n
    arr = [i, i + 1]
    acc = acc + arr[0] + arr[1]
    i = i + 1
  end
  acc
end
p mixed(500)

# linked list pattern (= conservative scanner には厳しい)
def cons(n)
  list = nil
  i = 0
  while i < n
    list = [i, list]
    i = i + 1
  end
  list
end
l = cons(500)
# walk to count
def llen(l)
  n = 0
  while l != nil
    n = n + 1
    l = l[1]
  end
  n
end
p llen(l)
