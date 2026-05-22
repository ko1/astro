# int 演算 / 比較

p 1 + 2
p 10 - 3
p 4 * 5
p 20 / 6
p 17 % 5
p 1 << 4
p (-3)

# 比較
p 1 < 2
p 2 < 1
p 3 == 3
p 3 != 3
p 3 <= 3
p 3 >= 4

# 大きい数 (= 63 bit fixnum 範囲内)
p 1000000 * 1000
p 999999 + 999999

# 負数
p (-5) + 3
p (-5) * (-5)
p (-10) / 3
p (-7) % 3

# spaceship
p 1 <=> 2
p 2 <=> 2
p 2 <=> 1

# augmented assignment (= PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE).
# 過去 iter 72 で chain bump 忘れで `+=` が壊れた (= 43 instead of
# 50M×43 などの type mismatch) ことの regression test。
a = 10
a += 1
p a       # 11
a -= 3
p a       # 8
a *= 4
p a       # 32
a /= 5
p a       # 6
a %= 4
p a       # 2

# 連続 += loop
sum = 0
i = 0
while i < 100
  sum += i
  i += 1
end
p sum     # 4950
p i       # 100
