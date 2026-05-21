# array operations

a = [1, 2, 3]
p a
p a.size
p a[0]
p a[2]
p a[-1]

# push
a.push(4)
a.push(5)
p a
p a.size

# set
a[0] = 100
p a

# pop
p a.pop
p a

# empty
p []
p [].size

# nested
p [[1, 2], [3, 4]]
p [[1, 2], [3, 4]][0]
p [[1, 2], [3, 4]][0][1]

# plus (concat)
p [1, 2] + [3, 4]
p [] + [1]
p [1] + []

# repeat
p [1, 2] * 3
p [] * 5

# size growth (= forces realloc multiple times)
b = []
i = 0
while i < 20
  b.push(i)
  i = i + 1
end
p b
p b.size

# mixed types
p [1, "a", true, nil, [2]]

# array of strings
ss = ["foo", "bar", "baz"]
p ss
p ss[1]
p ss.size

# array eq
p [1, 2, 3] == [1, 2, 3]
p [1, 2, 3] != [1, 2, 4]
p [] == []
p [1, [2, 3]] == [1, [2, 3]]
