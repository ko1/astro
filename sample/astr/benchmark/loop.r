# Tight integer-loop benchmark — exercises lget/lset and node_while
# without the function-call overhead that fib emphasises.
sum <- 0
i <- 1
while (i <= 50000000) {
    sum <- sum + i
    i <- i + 1
}
print(sum)
