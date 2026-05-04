# expect: 55
# while + for loops
sum <- 0
i <- 1
while (i <= 10) {
    sum <- sum + i
    i <- i + 1
}
print(sum)
