# 大きい alloc 単発 + 長く保持

# 大きい配列を作って維持
big = []
i = 0
while i < 5000
  big.push(i * 2)
  i = i + 1
end
p big.size
p big[0]
p big[2500]
p big[4999]

# 長い文字列
def repeat_str(s, n)
  r = ""
  i = 0
  while i < n
    r = r + s
    i = i + 1
  end
  r
end
big_str = repeat_str("abc", 200)  # 600 chars
p big_str.size

# 配列の配列 (nested)
matrix = []
i = 0
while i < 30
  row = []
  j = 0
  while j < 30
    row.push(i * 30 + j)
    j = j + 1
  end
  matrix.push(row)
  i = i + 1
end
p matrix.size
p matrix[15].size
p matrix[15][15]
p matrix[29][29]
