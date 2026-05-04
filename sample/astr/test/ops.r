# expect: 1
# operator coverage: %%, %/%, ^, comparisons, logicals
a <- 17 %% 5         # 2
b <- 17 %/% 5        # 3
c <- 2 ^ 10          # 1024
ok <- (a == 2) && (b == 3) && (c == 1024)
ok <- ok && (a < b) && (b <= 3) && !(a > b)
print(if (ok) 1 else 0)
